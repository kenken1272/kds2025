from __future__ import annotations
import os
import time
import logging
from typing import Optional

# --- optional deps ---
try:
    import serial  # /dev/tty* 用
except Exception:
    serial = None  # type: ignore

try:
    from PIL import Image, ImageDraw, ImageFont
except Exception:
    Image = None
    ImageDraw = None
    ImageFont = None

try:
    import qrcode
except Exception:
    qrcode = None
# ---------------------

logger = logging.getLogger("atomprinter.printer")


class PrinterAdapter:
    """
    - /dev/usb/lp* (usblp): 書く時だけ open→write→close（排他衝突を避ける）
    - /dev/tty*          : pyserial で継続運用
    """

    def __init__(self, device: str = "/dev/usb/lp0", baud: int = 9600, timeout: float = 1.0):
        self.device = device
        self.baud = int(baud)
        self.timeout = float(timeout)
        self._ser: Optional["serial.Serial"] = None  # tty 用のみ
        # usblp は都度 open なのでここで開かない
        if not self._is_usblp():
            self._open_tty()

    # -------- 内部ヘルパ --------
    def _is_usblp(self) -> bool:
        # 例: /dev/usb/lp0
        return "/usb/lp" in self.device

    def _open_tty(self) -> None:
        if self._ser:
            return
        if serial is None:
            raise RuntimeError("pyserial is not available but a tty device was requested")
        self._ser = serial.Serial(self.device, self.baud, timeout=self.timeout)
        logger.info("opened tty device %s @%d", self.device, self.baud)

    def _write(self, data: bytes) -> None:
        if self._is_usblp():
            # usblp は排他なので、毎回短時間だけ開く
            with open(self.device, "wb", buffering=0) as f:
                f.write(data)
                f.flush()
        else:
            if not self._ser:
                self._open_tty()
            assert self._ser is not None
            self._ser.write(data)
            self._ser.flush()

    # -------- 公開API（互換層） --------
    def init(self) -> None:
        self._write(b"\x1B\x40")  # ESC @

    def text(self, text: str) -> None:
        payload = self._normalize_text_for_printer(text)
        if not payload.endswith(b"\n"):
            payload += b"\n"
        self._write(payload)

    def feed(self, n: int = 1) -> None:
        self._write(b"\n" * max(0, int(n)))

    def cut(self) -> None:
        # 一般的なカットコマンド（失敗しても致命ではない）
        try:
            self._write(b"\x1D\x56\x00")  # GS V 0
        except Exception:
            try:
                self._write(b"\x1B\x69")   # ESC i
            except Exception:
                logger.debug("cut not supported")

    def raw(self, data: bytes) -> None:
        self._write(data)

    def close(self) -> None:
        if self._ser:
            try:
                self._ser.close()
            finally:
                self._ser = None

    # -------- 文字列→バイト整形 --------
    def _normalize_text_for_printer(self, text: str | None) -> bytes:
        if not text:
            return b""
        t = text.replace("\u00A5", "\\")           # ¥ → \
        t = t.replace("\r\n", "\n").replace("\r", "\n")
        out = []
        for ch in t:
            o = ord(ch)
            if ch == "\n" or (0x20 <= o <= 0x7E):  # ASCII 可視 + LF
                out.append(ch)
            else:
                out.append("?")                    # 日本語は画像化で対応
        return "".join(out).encode("utf-8")

    # -------- 画像/QR（任意機能） --------
    def print_image(self, pil_img) -> None:
        if Image is None:
            raise RuntimeError("Pillow not available")
        # usblp/tty どちらでも同じ。ラスタ送信（GS v 0）
        img = pil_img.convert("L")
        DOT_WIDTH = 384
        w, h = img.size
        if w != DOT_WIDTH:
            img = img.resize((DOT_WIDTH, int(h * (DOT_WIDTH / w))), Image.LANCZOS)
            w, h = img.size

        bw = img.point(lambda p: 0 if p > 128 else 1, "1")
        width_bytes = (w + 7) // 8
        header = b"\x1D\x76\x30\x00"

        RASTER = 128
        for y0 in range(0, h, RASTER):
            band_h = min(RASTER, h - y0)
            xL, xH = width_bytes & 0xFF, (width_bytes >> 8) & 0xFF
            yL, yH = band_h & 0xFF, (band_h >> 8) & 0xFF
            self._write(header + bytes([xL, xH, yL, yH]))

            # 行ごとにビット詰め
            band = bytearray()
            for row in range(y0, y0 + band_h):
                row_bytes = bytearray(width_bytes)
                for x in range(w):
                    bit = 1 if bw.getpixel((x, row)) in (0, 1) else 0
                    if bit:
                        row_bytes[x >> 3] |= (0x80 >> (x & 7))
                band.extend(row_bytes)

            # 分割送信
            CHUNK = 4096
            for i in range(0, len(band), CHUNK):
                self._write(bytes(band[i:i + CHUNK]))
            time.sleep(0.01 + band_h * 0.002)

    def print_qrcode(self, content: str, size: int = 6) -> None:
        if qrcode is None or Image is None:
            raise RuntimeError("qrcode or Pillow not available")
        qr = qrcode.QRCode(border=1, box_size=size)
        qr.add_data(content)
        qr.make(fit=True)
        img = qr.make_image(fill_color="black", back_color="white").convert("L")
        self.print_image(img)
