"""Minimal FastAPI server to replace ESP32 webserver.

Features implemented here (minimal):
- Serve static files from project `data/www` (PWA frontend)
- /api/ping
- /api/orders (POST to enqueue a print job)
- WebSocket `/ws` that broadcasts simple events
- Background print worker that uses PrinterAdapter (rpi/printer.py)
"""
import asyncio
import os
import json
import uuid
import logging
from typing import List

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from dotenv import load_dotenv

from printer import PrinterAdapter

load_dotenv()

logger = logging.getLogger("uvicorn.error")

API_PORT = int(os.environ.get("API_PORT", "8000"))
SERIAL_DEV = os.environ.get("TTY_DEVICE", "/dev/ttyUSB-atomprinter")
SERIAL_BAUD = int(os.environ.get("BAUD", "9600"))

app = FastAPI()
# serve the frontend PWA from data/www
app.mount("/", StaticFiles(directory=os.path.join(os.path.dirname(__file__), "..", "data", "www"), html=True), name="static")


class WSManager:
    def __init__(self):
        self.active: List[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)

    def disconnect(self, ws: WebSocket):
        try:
            self.active.remove(ws)
        except ValueError:
            pass

    async def broadcast(self, message: dict):
        data = json.dumps(message, ensure_ascii=False)
        for ws in list(self.active):
            try:
                await ws.send_text(data)
            except Exception:
                self.disconnect(ws)


ws_manager = WSManager()
print_queue: asyncio.Queue = asyncio.Queue()


@app.on_event("startup")
async def startup_event():
    # open printer adapter and start background worker
    app.state.printer = PrinterAdapter(device=SERIAL_DEV, baud=SERIAL_BAUD)
    app.state.worker_task = asyncio.create_task(print_worker())
    logger.info("Startup complete: printer=%s baud=%s", SERIAL_DEV, SERIAL_BAUD)


@app.on_event("shutdown")
async def shutdown_event():
    task = getattr(app.state, "worker_task", None)
    if task:
        task.cancel()
    printer = getattr(app.state, "printer", None)
    if printer:
        printer.close()


@app.get("/api/ping")
async def api_ping():
    return JSONResponse({"ok": True})


@app.post("/api/orders")
async def api_orders(request: Request):
    body = await request.json()
    # attach an id
    order_id = str(uuid.uuid4())
    job = {"id": order_id, "order": body}
    await print_queue.put(job)
    # notify frontends
    await ws_manager.broadcast({"event": "order_enqueued", "id": order_id})
    return JSONResponse({"enqueued": True, "id": order_id})


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws_manager.connect(ws)
    try:
        while True:
            # simple echo of incoming messages; mostly we expect server->client pushes
            data = await ws.receive_text()
            logger.debug("Received WS message: %s", data)
    except WebSocketDisconnect:
        ws_manager.disconnect(ws)


async def print_worker():
    """Background worker that takes jobs from print_queue and uses PrinterAdapter.

    This minimal worker sends the raw JSON of the order to the printer
    via PrinterAdapter.print_text(). In real usage, you'd render text/bitmap
    similar to the ESP32 implementation.
    """
    printer: PrinterAdapter = app.state.printer
    loop = asyncio.get_running_loop()
    while True:
        job = await print_queue.get()
        job_id = job.get("id")
        try:
            text = json.dumps(job["order"], ensure_ascii=False, indent=0)
            # send blocking IO in executor
            await loop.run_in_executor(None, printer.print_text, text)
            await ws_manager.broadcast({"event": "printed", "id": job_id})
            logger.info("Printed job %s", job_id)
        except Exception as e:
            logger.exception("Printing failed for %s: %s", job_id, e)
            await ws_manager.broadcast({"event": "print_failed", "id": job_id, "error": str(e)})
        finally:
            print_queue.task_done()


if __name__ == "__main__":
    # helper for local run: `python rpi/app.py` will use uvicorn if installed
    import uvicorn

    uvicorn.run("rpi.app:app", host="0.0.0.0", port=API_PORT, log_level="info")
