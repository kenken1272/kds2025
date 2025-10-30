from fastapi import APIRouter, Body, HTTPException
from pydantic import BaseModel

router = APIRouter(prefix="/api", tags=["kds"])

@router.get("/health")
def health():
    return {"ok": True}

class TextReq(BaseModel):
    text: str

# GET/POST両対応（405対策）
@router.api_route("/print/text", methods=["GET","POST"])
def print_text(text: str | None = None, body: TextReq | None = Body(default=None)):
    payload = text if text is not None else (body.text if body else "")
    if not payload:
        raise HTTPException(status_code=400, detail="text is required")
    # app.state.printer に格納済みのアダプタを使用（app側で設定）
    from fastapi import Request
    # Request を使わず直接 import 循環を避けるため、後述の app.include_router 直後に setter を使う方法に変更
    raise HTTPException(status_code=500, detail="printer not wired")
