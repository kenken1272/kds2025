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
        return self._serial.write(data)

    def print_text(self, text: str) -> None:
        """Send a simple text print job. Sends ESC @ then text then LF.

        This is a minimal compatibility mode with ESC/POS printers.
        """
        # ESC @ reset
        esc_at = b"\x1b@"
        payload = text.replace("\r\n", "\n").encode("utf-8", errors="replace")
        # Ensure newline at end so paper advances
        if not payload.endswith(b"\n"):
            payload += b"\n"
        data = esc_at + payload
        logger.debug("Printing %d bytes to %s", len(data), self.device)
        self.send_bytes(data)
