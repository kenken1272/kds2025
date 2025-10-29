"""Minimal printer adapter using pyserial.

This module provides a small sync PrinterAdapter that can be used by an
async background worker (via run_in_executor) to send bytes to a serial
ESC/POS-style printer. It is intentionally minimal: open device, send
bytes, and a convenience print_text method that sends ESC @ then the text.
"""
import os
import time
import logging

try:
    import serial
except Exception:  # pragma: no cover - runtime environment may not have pyserial
    serial = None
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

logger = logging.getLogger("atomprinter.printer")


class PrinterAdapter:
    def __init__(self, device: str = "/dev/ttyUSB-atomprinter", baud: int = 9600, timeout: float = 1.0):
        self.device = device
        self.baud = int(baud)
        self.timeout = float(timeout)
        self._serial = None
        self._open()

    def _open(self):
        if serial is None:
            logger.warning("pyserial not installed or import failed; printer unavailable")
            return
        try:
            self._serial = serial.Serial(self.device, self.baud, timeout=self.timeout)
            # short pause to let device settle
            time.sleep(0.05)
            logger.info("Opened serial printer %s @ %d", self.device, self.baud)
        except Exception as e:
            logger.exception("Failed to open serial device %s: %s", self.device, e)
            self._serial = None

    def is_open(self) -> bool:
        return self._serial is not None and getattr(self._serial, "is_open", True)

    def close(self):
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None

    def send_bytes(self, data: bytes) -> int:
        """Write raw bytes to the serial device. Returns number of bytes written.

        Raises exception on failure.
        """
        if not self.is_open():
            # try reopen once
            self._open()
            if not self.is_open():
                raise RuntimeError(f"Serial device not available: {self.device}")

        # robust write with retries
        max_retries = 3
        backoff = 0.05
        total_written = 0
        for attempt in range(1, max_retries + 1):
            try:
                written = self._serial.write(data[total_written:])
                # pyserial returns number of bytes written
                if written is None:
                    # some backends may return None; assume full
                    total_written = len(data)
                else:
                    total_written += int(written)
                if total_written >= len(data):
                    return total_written
                # otherwise wait and retry remaining
                time.sleep(backoff)
            except Exception as e:
                logger.warning("Serial write attempt %d failed: %s", attempt, e)
                # try reopen
                try:
                    self._open()
                except Exception:
                    pass
                time.sleep(backoff * attempt)
        # if we get here, incomplete write
        raise RuntimeError(f"Failed to write all bytes to serial after {max_retries} attempts ({total_written}/{len(data)})")

    def print_text(self, text: str) -> None:
        """Send a simple text print job. Sends ESC @ then text then LF.

        This is a minimal compatibility mode with ESC/POS printers.
        """
        esc_at = b"\x1b@"
        payload = self._normalize_text_for_printer(text)
        # Ensure newline at end so paper advances
        if not payload.endswith(b"\n"):
            payload += b"\n"
        data = esc_at + payload
        logger.debug("Printing %d bytes to %s", len(data), self.device)
        self.send_bytes(data)

    def _normalize_text_for_printer(self, text: str) -> bytes:
        """Normalize text to send to a basic ESC/POS printer.

        - Replace U+00A5 (¥) with ASCII backslash `\\` (common mapping for many printers)
        - Keep ASCII printable and common whitespace; replace other characters with '?'
        - Encode as UTF-8 (many compact printers accept UTF-8) -- if you want CP932/CP437, convert here.
        """
        if text is None:
            return b""
        t = text.replace('\u00A5', '\\')
        # Normalize CRLF -> LF
        t = t.replace('\r\n', '\n').replace('\r', '\n')
        out_chars = []
        for ch in t:
            o = ord(ch)
            # keep LF and printable ASCII range
            if ch == '\n' or (0x20 <= o <= 0x7E):
                out_chars.append(ch)
            else:
                # non-ascii (e.g., Japanese) -> replace with '?'
                # For full Japanese receipts use print_image() to send rasterized image instead.
                out_chars.append('?')
        return ''.join(out_chars).encode('utf-8')

    def print_image(self, pil_img) -> None:
        """Send a Pillow Image as ESC/POS GS v 0 raster to printer.

        pil_img should be a Pillow Image instance. This will convert to monochrome
        and send as required by many thermal printers (width e.g. 384 dots).
        """
        if Image is None:
            raise RuntimeError("Pillow not available in environment")
        if not self.is_open():
            self._open()
            if not self.is_open():
                raise RuntimeError("Serial device not available")

        # target width in pixels (DOT_WIDTH). Many 80mm printers: 384 dots; make configurable later.
        DOT_WIDTH = 384
        img = pil_img.convert('L')
        w, h = img.size
        if w != DOT_WIDTH:
            # resize maintaining height proportion
            new_h = int(h * (DOT_WIDTH / w))
            img = img.resize((DOT_WIDTH, new_h), Image.LANCZOS)
            w, h = img.size

        # convert to 1-bit image (black=1)
        bw = img.point(lambda p: 0 if p > 128 else 1, '1')
        width_bytes = (w + 7) // 8

        # send in vertical bands to avoid huge single transfer
        RASTER_HEIGHT = 128  # rows per band; can be tuned per printer
        hdr_cmd = b'\x1D\x76\x30\x00'

        for y0 in range(0, h, RASTER_HEIGHT):
            band_h = min(RASTER_HEIGHT, h - y0)
            xL = width_bytes & 0xFF
            xH = (width_bytes >> 8) & 0xFF
            yL = band_h & 0xFF
            yH = (band_h >> 8) & 0xFF
            header = hdr_cmd + bytes([xL, xH, yL, yH])
            # send header
            self.send_bytes(header)

            # build band bytes row-major
            band_bytes = bytearray()
            for row in range(y0, y0 + band_h):
                row_bytes = bytearray(width_bytes)
                for x in range(w):
                    pixel = bw.getpixel((x, row))
                    bit = 1 if pixel == 0 or pixel == 1 else 0
                    if bit:
                        row_bytes[x >> 3] |= (0x80 >> (x & 7))
                band_bytes.extend(row_bytes)

            # send band in chunks
            chunk_size = 4096
            for i in range(0, len(band_bytes), chunk_size):
                self.send_bytes(bytes(band_bytes[i:i+chunk_size]))
            # small delay between bands
            time.sleep(0.01 + band_h * 0.002)

    def render_receipt_image(self, order: dict, logo_path: str = None, dot_width: int = 384, font_path: str = None):
        """Render a receipt PIL Image including optional logo and order details (supports Japanese).

        - order: dict with keys 'orderNo', 'items' (list of dicts with name, qty, unitPriceApplied/unitPrice)
        - logo_path: optional path to an image to place at the top (will be resized to fit width)
        - dot_width: target printer width in pixels (commonly 384)
        - font_path: path to a TTF/OTF font supporting Japanese (recommended: Noto Sans CJK JP)
        """
        if Image is None or ImageDraw is None or ImageFont is None:
            raise RuntimeError("Pillow not available")

        # choose font
        font_size = 22
        font = None
        try:
            if font_path:
                font = ImageFont.truetype(font_path, font_size)
            else:
                # try environment default fonts common on Linux
                for p in ['/usr/share/fonts/truetype/noto/NotoSansJP-Regular.otf', '/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc', '/usr/share/fonts/truetype/fonts-japanese-gothic.ttf']:
                    try:
                        font = ImageFont.truetype(p, font_size)
                        break
                    except Exception:
                        continue
        except Exception:
            font = None
        if font is None:
            font = ImageFont.load_default()

        # build text lines and wrap
        lines = []
        # header: store name if present in order
        store_name = order.get('storeName') or ''
        if store_name:
            lines.append(store_name)
        lines.append(f"Order No. {order.get('orderNo', '')}")
        lines.append('')

        items = order.get('items', [])
        for it in items:
            name = it.get('name') or it.get('sku') or ''
            qty = it.get('qty', 1)
            unit = it.get('unitPriceApplied') or it.get('unitPrice') or 0
            lines.append(f"{name}  x{qty}  {unit}yen")

        lines.append('')
        total = order.get('totalAmount')
        if total is None:
            t = 0
            for it in items:
                unit = it.get('unitPriceApplied') or it.get('unitPrice') or 0
                qty = it.get('qty', 1)
                discount = it.get('discountValue', 0)
                t += unit * qty - discount
            total = t
        lines.append(f"Total: {total} yen")
        footer = order.get('footerMessage','Thank you!')
        lines.append(footer)

        # helper: wrap text to fit width
        def wrap_text(text, max_width_px):
            if not text:
                return ['']
            is_cjk = any('\u4e00' <= ch <= '\u9fff' for ch in text)
            if is_cjk:
                tokens = list(text)
            else:
                tokens = text.split(' ')
            out_lines = []
            cur = ''
            for token in tokens:
                if is_cjk:
                    candidate = cur + token
                else:
                    candidate = cur + ((' ' + token) if cur else token)
                w = font.getsize(candidate)[0]
                if w <= max_width_px:
                    cur = candidate
                else:
                    if cur:
                        out_lines.append(cur)
                    # if single token too long, split by character
                    if font.getsize(token)[0] > max_width_px:
                        acc = ''
                        for ch in token:
                            if font.getsize(acc + ch)[0] <= max_width_px:
                                acc += ch
                            else:
                                if acc:
                                    out_lines.append(acc)
                                acc = ch
                        if acc:
                            cur = acc
                        else:
                            cur = ''
                    else:
                        cur = token
            if cur:
                out_lines.append(cur)
            return out_lines

        # measure height
        line_height = font.getsize('Ay')[1] + 6
        max_text_width = dot_width - 16

        # apply wrapping to initial lines
        wrapped_lines = []
        for ln in lines:
            wrapped_lines.extend(wrap_text(ln, max_text_width))
        lines = wrapped_lines
        logo_h = 0
        logo_img = None
        if logo_path and os.path.exists(logo_path):
            try:
                logo_img = Image.open(logo_path).convert('L')
                # resize logo to fit width
                lw, lh = logo_img.size
                if lw != dot_width:
                    new_h = int(lh * (dot_width / lw))
                    logo_img = logo_img.resize((dot_width, new_h), Image.LANCZOS)
                logo_h = logo_img.size[1]
            except Exception:
                logo_img = None
                logo_h = 0

        img_h = logo_h + max(200, len(lines) * line_height + 40)
        img = Image.new('L', (dot_width, img_h), 255)
        draw = ImageDraw.Draw(img)

        y = 0
        if logo_img:
            img.paste(logo_img, (0, 0))
            y += logo_h + 8

        for ln in lines:
            draw.text((8, y), ln, font=font, fill=0)
            y += line_height

        # crop to used height
        img = img.crop((0, 0, dot_width, y + 12))
        return img

    def print_receipt(self, order: dict, logo_path: str = None, font_path: str = None, dot_width: int = 384) -> None:
        """Render receipt and send to printer as raster image."""
        if Image is None:
            raise RuntimeError("Pillow not available")
        img = self.render_receipt_image(order, logo_path=logo_path, dot_width=dot_width, font_path=font_path)
        # print image in bands to avoid huge memory on some printers
        self.print_image(img)

    def print_qrcode(self, content: str, size: int = 6) -> None:
        """Generate QR code image and print via print_image.

        Uses qrcode library to produce an image then sends via print_image.
        """
        if qrcode is None or Image is None:
            raise RuntimeError("qrcode or Pillow not available")
        qr = qrcode.QRCode(border=1, box_size=size)
        qr.add_data(content)
        qr.make(fit=True)
        img = qr.make_image(fill_color='black', back_color='white').convert('L')
        # center-crop or pad to DOT_WIDTH as necessary
        self.print_image(img)
