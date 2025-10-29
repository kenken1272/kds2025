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
import time
from typing import List

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, Response, Query
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from dotenv import load_dotenv

from printer import PrinterAdapter
from store import S, wal_append, snapshot_save, recover_to_latest, generate_order_no, enqueue_print, find_order, archive_find_order, archive_order_and_remove, get_pending_print_jobs, generate_sku_main, generate_sku_side

load_dotenv()

logger = logging.getLogger("uvicorn.error")

API_PORT = int(os.environ.get("API_PORT", "8000"))
SERIAL_DEV = os.environ.get("TTY_DEVICE", "/dev/ttyUSB-atomprinter")
SERIAL_BAUD = int(os.environ.get("BAUD", "9600"))

app = FastAPI()
# serve the frontend PWA from data/www
app.mount("/", StaticFiles(directory=os.path.join(os.path.dirname(__file__), "data", "www"), html=True), name="static")


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
    return JSONResponse({"ok": True, "ip": os.environ.get('API_HOST', '0.0.0.0')})


@app.get('/api/menu')
async def api_menu():
    # return menu and ETag-like catalogVersion
    return JSONResponse({
        'catalogVersion': S.settings.get('catalogVersion', 1),
        'menu': S.menu
    })


@app.get('/api/state')
async def api_state(light: int = Query(0)):
    # light=1 returns compact orders
    S.create_initial_menu_if_empty()
    light = int(light)
    menu = [] if light else S.menu
    orders = S.orders[-60:] if light else S.orders
    return JSONResponse({
        'settings': S.settings,
        'session': S.session,
        'printer': S.printer,
        'menu': menu,
        'orders': orders
    })


@app.post('/api/orders/update')
async def api_orders_update(request: Request):
    body = await request.json()
    orderNo = body.get('orderNo')
    newStatus = body.get('status')
    if not orderNo:
        return JSONResponse({'error':'Missing orderNo'}, status_code=400)
    found = False
    for o in S.orders:
        if o.get('orderNo') == orderNo:
            if newStatus:
                o['status'] = newStatus
            found = True
            wal_append(json.dumps({"ts": int(time.time()), "action": "ORDER_UPDATE", "orderNo": orderNo, "status": newStatus}, ensure_ascii=False))
            break
    if not found:
        return JSONResponse({'error':'Order not found'}, status_code=404)
    snapshot_save()
    await ws_manager.broadcast({"type":"order.updated","orderNo":orderNo,"status":newStatus})
    return JSONResponse({'ok':True})


@app.get('/api/orders/detail')
async def api_order_detail(orderNo: str = Query(None)):
    if not orderNo:
        return JSONResponse({'error':'Missing orderNo'}, status_code=400)
    o = find_order(orderNo)
    if not o:
        return JSONResponse({'error':'Order not found'}, status_code=404)
    # compute total
    total = 0
    items = o.get('items', [])
    for it in items:
        unit = it.get('unitPriceApplied') or it.get('unitPrice') or 0
        qty = it.get('qty') or 1
        discount = it.get('discountValue', 0)
        lineTotal = unit * qty - discount
        total += lineTotal
    resp = { 'orderNo': o.get('orderNo'), 'status': o.get('status'), 'ts': o.get('ts'), 'printed': o.get('printed'), 'items': items, 'totalAmount': total }
    return JSONResponse(resp)


@app.get('/api/orders/archive')
async def api_orders_archive(sessionId: str = Query(None)):
    # simple stream of archived orders
    data = {
        'sessionId': sessionId or S.session.get('sessionId',''),
        'orders': S.archived_orders
    }
    return JSONResponse(data)


@app.post('/api/orders/{orderNo}/cooked')
async def api_order_cooked(orderNo: str):
    o = find_order(orderNo)
    if not o:
        return JSONResponse({'error':'Order not found'}, status_code=404)
    o['cooked'] = True
    o['pickup_called'] = True
    wal_append(json.dumps({"ts": int(time.time()), "action": "ORDER_COOKED", "orderNo": orderNo}, ensure_ascii=False))
    snapshot_save()
    await ws_manager.broadcast({"type":"order.cooked","orderNo":orderNo})
    return JSONResponse({'ok':True})


@app.post('/api/orders/{orderNo}/picked')
async def api_order_picked(orderNo: str):
    o = find_order(orderNo)
    if not o:
        return JSONResponse({'error':'Order not found'}, status_code=404)
    o['picked_up'] = True
    o['pickup_called'] = False
    # move to archive
    ok = archive_order_and_remove(orderNo)
    wal_append(json.dumps({"ts": int(time.time()), "action": "ORDER_PICKED", "orderNo": orderNo}, ensure_ascii=False))
    snapshot_save()
    await ws_manager.broadcast({"type":"order.picked","orderNo":orderNo})
    return JSONResponse({'ok': ok})


@app.get('/api/printer/status')
async def api_printer_status():
    return JSONResponse({
        'paperOut': S.printer.get('paperOut', False),
        'overheat': S.printer.get('overheat', False),
        'holdJobs': S.printer.get('holdJobs', 0),
        'pendingJobs': get_pending_print_jobs()
    })


@app.post('/api/printer/paper-replaced')
async def api_printer_paper_replaced():
    S.printer['paperOut'] = False
    await ws_manager.broadcast({'type':'printer.status','paperOut': False, 'holdJobs': S.printer.get('holdJobs',0)})
    return JSONResponse({'ok':True})


@app.post('/api/print/test-jp')
async def api_print_test_jp():
    # simple Japanese test: print ASCII placeholder (real Japanese uses raster)
    try:
        app.state.printer.print_text('== 日本語テスト ==')
        app.state.printer.print_text('注文番号 0001')
        return JSONResponse({'ok':True})
    except Exception as e:
        return JSONResponse({'ok':False, 'error': str(e)}, status_code=500)


@app.post('/api/logo')
async def api_upload_logo(file: bytes = None, request: Request = None):
    """Upload logo via multipart/form-data (field name 'file'). Returns URL to logo."""
    # FastAPI prefers using starlette UploadFile, but to keep changes minimal parse body manually
    # Use request.form() to access UploadFile
    form = await request.form()
    upload = form.get('file')
    if not upload:
        return JSONResponse({'error':'Missing file'}, status_code=400)
    try:
        storage_dir = os.path.join(os.path.dirname(__file__), 'storage')
        os.makedirs(storage_dir, exist_ok=True)
        dest = os.path.join(storage_dir, 'logo.png')
        with open(dest, 'wb') as f:
            content = await upload.read()
            f.write(content)
        return JSONResponse({'ok':True, 'url': '/api/logo'})
    except Exception as e:
        return JSONResponse({'error': str(e)}, status_code=500)


@app.get('/api/logo')
async def api_get_logo():
    p = os.path.join(os.path.dirname(__file__), 'storage', 'logo.png')
    if not os.path.exists(p):
        return JSONResponse({'error':'logo not found'}, status_code=404)
    from fastapi.responses import FileResponse
    return FileResponse(p, media_type='image/png')


@app.post('/api/print/receipt')
async def api_print_receipt(body: dict):
    # body expected to contain orderNo
    orderNo = body.get('orderNo') if isinstance(body, dict) else None
    if not orderNo:
        return JSONResponse({'error':'Missing orderNo'}, status_code=400)
    o = find_order(orderNo)
    if not o:
        return JSONResponse({'error':'Order not found'}, status_code=404)
    logo_path = os.path.join(os.path.dirname(__file__), 'storage', 'logo.png')
    try:
        # call print_receipt in executor
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, app.state.printer.print_receipt, o, logo_path, os.environ.get('FONT_PATH'))
        await ws_manager.broadcast({'event':'printed','orderNo':orderNo})
        return JSONResponse({'ok':True})
    except Exception as e:
        return JSONResponse({'ok':False,'error':str(e)}, status_code=500)


@app.get('/api/sales/summary')
async def api_sales_summary(rebuild: int = Query(0)):
    # minimal sales summary
    return JSONResponse({
        'sessionId': S.session.get('sessionId',''),
        'updatedAt': S.sales_summary.get('lastUpdated'),
        'confirmedOrders': S.sales_summary.get('confirmedOrders',0),
        'cancelledOrders': S.sales_summary.get('cancelledOrders',0),
        'totalOrders': S.sales_summary.get('confirmedOrders',0) + S.sales_summary.get('cancelledOrders',0),
        'netSales': S.sales_summary.get('revenue',0),
        'cancelledAmount': S.sales_summary.get('cancelledAmount',0),
        'grossSales': S.sales_summary.get('revenue',0) + S.sales_summary.get('cancelledAmount',0),
        'currency': 'JPY'
    })


@app.get('/api/export/snapshot')
async def api_export_snapshot():
    p = os.path.join(os.path.dirname(__file__), 'storage', 'snapshot.json')
    if not os.path.exists(p):
        return JSONResponse({'error':'snapshot not found'}, status_code=404)
    def iterfile():
        with open(p, 'rb') as f:
            while True:
                chunk = f.read(8192)
                if not chunk:
                    break
                yield chunk
    return StreamingResponse(iterfile(), media_type='application/json')


@app.post('/api/products/main')
async def api_products_main(request: Request):
    body = await request.json()
    items = body.get('items', [])
    touched = False
    for v in items:
        sku = v.get('id') or generate_sku_main()
        name = v.get('name','')
        nameRomaji = v.get('nameRomaji','')
        price_normal = v.get('price_normal',0)
        presale_discount_amount = v.get('presale_discount_amount',0)
        active = v.get('active', True)
        existing = None
        for it in S.menu:
            if it.get('sku') == sku:
                existing = it
                break
        if existing:
            existing.update({'name':name,'nameRomaji':nameRomaji,'price_normal':price_normal,'presale_discount_amount':presale_discount_amount,'active':active})
        else:
            S.menu.append({'sku':sku,'name':name,'nameRomaji':nameRomaji,'category':'MAIN','active':active,'price_normal':price_normal,'presale_discount_amount':presale_discount_amount,'price_single':price_normal,'price_as_side':0})
        touched = True
        wal_append(json.dumps({"ts": int(time.time()), "action": "MAIN_UPSERT", "sku": sku, "name": name, "nameRomaji": nameRomaji, "price_normal": price_normal, "presale_discount_amount": presale_discount_amount, "active": active}, ensure_ascii=False))
    if touched:
        S.settings['catalogVersion'] = S.settings.get('catalogVersion',1) + 1
        snapshot_save()
    return JSONResponse({'ok':True})


@app.post('/api/products/side')
async def api_products_side(request: Request):
    body = await request.json()
    items = body.get('items', [])
    touched = False
    for v in items:
        sku = v.get('id') or generate_sku_side()
        name = v.get('name','')
        nameRomaji = v.get('nameRomaji','')
        price_single = v.get('price_single',0)
        price_as_side = v.get('price_as_side',0)
        active = v.get('active', True)
        existing = None
        for it in S.menu:
            if it.get('sku') == sku:
                existing = it
                break
        if existing:
            existing.update({'name':name,'nameRomaji':nameRomaji,'price_single':price_single,'price_as_side':price_as_side,'active':active})
        else:
            S.menu.append({'sku':sku,'name':name,'nameRomaji':nameRomaji,'category':'SIDE','active':active,'price_single':price_single,'price_as_side':price_as_side})
        touched = True
        wal_append(json.dumps({"ts": int(time.time()), "action": "SIDE_UPSERT", "sku": sku, "name": name, "nameRomaji": nameRomaji, "price_single": price_single, "price_as_side": price_as_side, "active": active}, ensure_ascii=False))
    if touched:
        S.settings['catalogVersion'] = S.settings.get('catalogVersion',1) + 1
        snapshot_save()
    return JSONResponse({'ok':True})


@app.post('/api/settings/chinchiro')
async def api_settings_chinchiro(request: Request):
    body = await request.json()
    S.settings.setdefault('chinchiro', {})
    if 'enabled' in body:
        S.settings['chinchiro']['enabled'] = body['enabled']
    if 'rounding' in body:
        S.settings['chinchiro']['rounding'] = body['rounding']
    if 'multipliers' in body:
        S.settings['chinchiro']['multipliers'] = body['multipliers']
    wal_append(json.dumps({"ts": int(time.time()), "action": "SETTINGS_UPDATE", "chinchiro": S.settings['chinchiro']}, ensure_ascii=False))
    snapshot_save()
    await ws_manager.broadcast({'type':'sync.snapshot'})
    return JSONResponse({'ok':True})


@app.post('/api/settings/qrprint')
async def api_settings_qrprint(request: Request):
    body = await request.json()
    S.settings.setdefault('qrPrint', {})
    if 'enabled' in body:
        S.settings['qrPrint']['enabled'] = body['enabled']
    if 'content' in body:
        S.settings['qrPrint']['content'] = body['content']
    wal_append(json.dumps({"ts": int(time.time()), "action": "SETTINGS_UPDATE", "qrPrint": S.settings['qrPrint']}, ensure_ascii=False))
    snapshot_save()
    await ws_manager.broadcast({'type':'sync.snapshot'})
    return JSONResponse({'ok':True})


@app.post('/api/network/ap-cycle')
async def api_network_ap_cycle(request: Request):
    body = await request.json() if request.headers.get('content-type','').startswith('application/json') else {}
    resumeAfter = int(body.get('resumeAfter', 60))
    # On Pi we don't control hostapd here; caller should run scripts. We accept request and report state.
    res = {'ok': True, 'apActive': True, 'resumeInSec': resumeAfter}
    return JSONResponse(res)


@app.get('/api/export/csv')
async def api_export_csv():
    # export current orders as CSV
    import io, csv
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(['orderNo','status','ts','totalAmount'])
    for o in S.orders:
        total = 0
        for it in o.get('items', []):
            unit = it.get('unitPriceApplied') or it.get('unitPrice') or 0
            qty = it.get('qty') or 1
            discount = it.get('discountValue', 0)
            total += unit * qty - discount
        writer.writerow([o.get('orderNo'), o.get('status'), o.get('ts'), total])
    data = output.getvalue().encode('utf-8')
    return Response(content=data, media_type='text/csv', headers={'Content-Disposition':'attachment; filename="orders.csv"'})


@app.get('/api/export/sales-summary-lite')
async def api_export_sales_summary_lite():
    doc = {
        'sessionId': S.session.get('sessionId',''),
        'generatedAt': int(time.time()),
        'lastUpdated': S.sales_summary.get('lastUpdated'),
        'confirmedOrders': S.sales_summary.get('confirmedOrders',0),
        'cancelledOrders': S.sales_summary.get('cancelledOrders',0),
        'totalOrders': S.sales_summary.get('confirmedOrders',0) + S.sales_summary.get('cancelledOrders',0),
        'netSales': S.sales_summary.get('revenue',0),
        'cancelledAmount': S.sales_summary.get('cancelledAmount',0),
        'grossSales': S.sales_summary.get('revenue',0) + S.sales_summary.get('cancelledAmount',0),
        'currency': 'JPY'
    }
    body = json.dumps(doc, ensure_ascii=False)
    return Response(content=body.encode('utf-8'), media_type='application/json', headers={'Content-Disposition':'attachment; filename="sales-summary-lite.json"'})


@app.get('/api/system/memory')
async def api_system_memory():
    # best-effort memory info
    try:
        import psutil
        mem = psutil.virtual_memory()
        return JSONResponse({'freeHeap': int(mem.available), 'total': int(mem.total)})
    except Exception:
        return JSONResponse({'freeHeap': 0, 'total': 0})


@app.post('/api/recover')
async def api_recover():
    ok, msg = recover_to_latest()
    if ok:
        await ws_manager.broadcast({'type':'sync.snapshot'})
        return JSONResponse({'ok':True, 'lastTs': msg})
    else:
        return JSONResponse({'ok':False, 'error': msg}, status_code=500)


@app.post('/api/time/set')
async def api_time_set(request: Request):
    body = await request.json()
    epoch = int(body.get('epoch', 0))
    if epoch <= 0:
        return JSONResponse({'error':'Missing epoch'}, status_code=400)
    # Attempt to set system time (may require sudo)
    try:
        import subprocess
        ts = time.gmtime(epoch)
        formatted = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(epoch))
        # requires sudo privileges on Pi; best-effort
        subprocess.run(['sudo','date','-s', formatted], check=True)
        return JSONResponse({'ok':True})
    except Exception as e:
        return JSONResponse({'ok':False, 'error': str(e)}, status_code=500)


@app.post('/api/settings/system')
async def api_settings_system(request: Request):
    body = await request.json()
    if 'presaleEnabled' in body:
        S.settings['presaleEnabled'] = bool(body['presaleEnabled'])
    if 'store' in body:
        st = body['store']
        S.settings['store'].update({k:st.get(k, S.settings['store'].get(k)) for k in ['name','nameRomaji','registerId']})
    if 'numbering' in body:
        num = body['numbering']
        S.settings.setdefault('numbering', {})
        if 'min' in num: S.settings['numbering']['min'] = num['min']
        if 'max' in num: S.settings['numbering']['max'] = num['max']
    snapshot_save()
    return JSONResponse({'ok':True})


@app.post('/api/session/end')
async def api_session_end():
    S.orders.clear()
    S.session['exported'] = False
    S.session['nextOrderSeq'] = 1
    S.session['sessionId'] = ''
    S.session['startedAt'] = int(time.time())
    S.printer['paperOut'] = False
    S.printer['overheat'] = False
    S.printer['holdJobs'] = 0
    snapshot_save()
    wal_append(json.dumps({"ts": int(time.time()), "action": "SESSION_END"}, ensure_ascii=False))
    await ws_manager.broadcast({'type':'session.ended'})
    return JSONResponse({'ok':True})


@app.post('/api/system/reset')
async def api_system_reset():
    # clear storage files and reset runtime structures
    try:
        p_wal = os.path.join(os.path.dirname(__file__), 'storage', 'wal.log')
        p_snap = os.path.join(os.path.dirname(__file__), 'storage', 'snapshot.json')
        try:
            os.remove(p_wal)
        except Exception:
            pass
        try:
            os.remove(p_snap)
        except Exception:
            pass
        S.menu.clear(); S.orders.clear(); S.archived_orders.clear()
        S.session = {'sessionId':'','startedAt': int(time.time()), 'exported': False, 'nextOrderSeq':1}
        S.printer = {'paperOut': False, 'overheat': False, 'holdJobs': 0}
        snapshot_save()
        wal_append(json.dumps({"ts": int(time.time()), "action": "SYSTEM_RESET"}, ensure_ascii=False))
        await ws_manager.broadcast({'type':'system.reset'})
        return JSONResponse({'ok':True, 'message':'System reset'})
    except Exception as e:
        return JSONResponse({'ok':False, 'error': str(e)}, status_code=500)


@app.get('/api/call-list')
async def api_call_list():
    lst = []
    for o in S.orders:
        if o.get('pickup_called'):
            lst.append({'orderNo': o.get('orderNo'), 'ts': o.get('ts')})
    return JSONResponse({'callList': lst})


@app.get('/debug/hello')
async def debug_hello():
    # simple debug page to trigger hello print via browser
    try:
        app.state.printer.print_text('HELLO WORLD')
        return Response("<html><body><h1>Printed</h1></body></html>", media_type='text/html')
    except Exception as e:
        return Response(f"<html><body><h1>Error: {e}</h1></body></html>", media_type='text/html')


@app.post("/api/orders")
async def api_orders(request: Request):
    body = await request.json()
    # handle reprint/cancel by path suffix
    path = str(request.url.path)
    S.create_initial_menu_if_empty()

    if path.endswith('/reprint'):
        orderNo = body.get('orderNo')
        if not orderNo:
            return JSONResponse({"error": "Missing orderNo"}, status_code=400)
        o = find_order(orderNo)
        if not o:
            return JSONResponse({"error": "Order not found"}, status_code=404)
        enqueue_print(o)
        await ws_manager.broadcast({"type":"order.created","orderNo": orderNo})
        return JSONResponse({"ok": True, "orderNo": orderNo})

    if path.endswith('/cancel'):
        orderNo = body.get('orderNo')
        reason = body.get('reason', '')
        if not orderNo:
            return JSONResponse({"error": "Missing orderNo"}, status_code=400)
        o = find_order(orderNo)
        if not o:
            return JSONResponse({"error": "Order not found"}, status_code=404)
        o['status'] = 'CANCELLED'
        o['cancelReason'] = reason
        wal_append(json.dumps({"ts": int(time.time()), "action": "ORDER_CANCEL", "orderNo": orderNo, "cancelReason": reason}, ensure_ascii=False))
        snapshot_save()
        await ws_manager.broadcast({"type": "order.updated", "orderNo": orderNo, "status": "CANCELLED"})
        return JSONResponse({"ok": True, "orderNo": orderNo})

    # create order
    order = body
    order_no = generate_order_no()
    order_obj = {
        'orderNo': order_no,
        'status': 'CREATED',
        'ts': int(time.time()),
        'printed': False,
        'cooked': False,
        'pickup_called': False,
        'picked_up': False,
        'items': order.get('lines', []) if isinstance(order.get('lines', []), list) else order.get('items', [])
    }
    S.orders.append(order_obj)
    wal_append(json.dumps({"ts": int(time.time()), "action": "ORDER_CREATE", "orderNo": order_no, "order": order_obj}, ensure_ascii=False))
    enqueue_print(order_obj)
    snapshot_save()
    await ws_manager.broadcast({"type": "order.created", "orderNo": order_no})
    return JSONResponse({"orderNo": order_no})


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
            # prefer raster receipt printing when possible
            printer = app.state.printer
            job_order = job.get('order')
            logo_path = os.path.join(os.path.dirname(__file__), 'storage', 'logo.png')
            try:
                await loop.run_in_executor(None, printer.print_receipt, job_order, logo_path, os.environ.get('FONT_PATH'))
                await ws_manager.broadcast({"event": "printed", "id": job_id})
                logger.info("Printed job %s (raster)", job_id)
            except Exception:
                # fallback to text printing
                await loop.run_in_executor(None, printer.print_text, text)
                await ws_manager.broadcast({"event": "printed", "id": job_id})
                logger.info("Printed job %s (text fallback)", job_id)
        except Exception as e:
            logger.exception("Printing failed for %s: %s", job_id, e)
            await ws_manager.broadcast({"event": "print_failed", "id": job_id, "error": str(e)})
        finally:
            print_queue.task_done()


if __name__ == "__main__":
    # helper for local run: `python rpi/app.py` will use uvicorn if installed
    import uvicorn

    uvicorn.run("rpi.app:app", host="0.0.0.0", port=API_PORT, log_level="info")
