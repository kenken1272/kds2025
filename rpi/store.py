"""Lightweight in-memory store with WAL and snapshot support.

This provides a minimal subset of the ESP32 `store` functions used by the
frontend and server routes. It's intentionally simple: data persisted as
JSON snapshot and a WAL file under `rpi/storage/`.
"""
import os
import json
import time
from threading import RLock
from typing import List, Dict, Optional, Tuple

ROOT = os.path.dirname(__file__)
STORAGE_DIR = os.path.join(ROOT, 'storage')
os.makedirs(STORAGE_DIR, exist_ok=True)

_lock = RLock()

class Store:
    def __init__(self):
        # runtime structures
        self.menu: List[Dict] = []
        self.orders: List[Dict] = []
        self.archived_orders: List[Dict] = []
        self.settings = {
            'catalogVersion': 1,
            'chinchiro': {'enabled': False, 'multipliers': [1.0, 2.0], 'rounding': 0},
            'store': {'name': 'My Store', 'nameRomaji': 'My Store', 'registerId': 'R1'},
            'presaleEnabled': False,
            'qrPrint': {'enabled': False, 'content': ''},
            'numbering': {'min': 1, 'max': 9999},
        }
        now = int(time.time())
        self.session = {'sessionId': '', 'startedAt': now, 'exported': False, 'nextOrderSeq': 1}
        self.printer = {'paperOut': False, 'overheat': False, 'holdJobs': 0}
        self.sales_summary = {
            'lastUpdated': now,
            'confirmedOrders': 0,
            'cancelledOrders': 0,
            'revenue': 0,
            'cancelledAmount': 0,
        }
        # SKU counters for generated SKUs
        self.next_sku_main = 1
        self.next_sku_side = 1

    def create_initial_menu_if_empty(self):
        if not self.menu:
            self.menu = [
                {
                    'sku': 'M001',
                    'name': 'Teriyaki Burger',
                    'nameRomaji': 'Teriyaki Burger',
                    'category': 'MAIN',
                    'active': True,
                    'price_normal': 800,
                    'price_presale': 700,
                    'presale_discount_amount': 0,
                    'price_single': 800,
                    'price_as_side': 0,
                },
                {
                    'sku': 'S001',
                    'name': 'Fries',
                    'nameRomaji': 'Fries',
                    'category': 'SIDE',
                    'active': True,
                    'price_single': 200,
                    'price_as_side': 200,
                },
            ]
            self.next_sku_main = 2
            self.next_sku_side = 2

    def ensure_sales_summary_defaults(self):
        now = int(time.time())
        self.sales_summary.setdefault('lastUpdated', now)
        self.sales_summary.setdefault('confirmedOrders', 0)
        self.sales_summary.setdefault('cancelledOrders', 0)
        self.sales_summary.setdefault('revenue', 0)
        self.sales_summary.setdefault('cancelledAmount', 0)

    def ensure_session_defaults(self):
        self.session.setdefault('sessionId', '')
        self.session.setdefault('startedAt', int(time.time()))
        self.session.setdefault('exported', False)
        self.session.setdefault('nextOrderSeq', 1)

S = Store()

def _wal_path():
    return os.path.join(STORAGE_DIR, 'wal.log')

def _snapshot_path():
    return os.path.join(STORAGE_DIR, 'snapshot.json')

def wal_append(line: str):
    with _lock:
        path = _wal_path()
        with open(path, 'a', encoding='utf-8') as f:
            f.write(line.rstrip('\n') + '\n')

def snapshot_save() -> bool:
    with _lock:
        try:
            data = {
                'menu': S.menu,
                'orders': S.orders,
                'archived_orders': S.archived_orders,
                'settings': S.settings,
                'session': S.session,
                'printer': S.printer,
                'sales_summary': S.sales_summary,
                'next_sku_main': S.next_sku_main,
                'next_sku_side': S.next_sku_side,
            }
            with open(_snapshot_path(), 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False)
            return True
        except Exception:
            return False

def recover_to_latest() -> Tuple[bool, Optional[str]]:
    """Attempt to recover state from latest snapshot and then replay WAL.

    Returns (ok, message) where message contains error text on failure or lastTs on success.
    """
    with _lock:
        try:
            if os.path.exists(_snapshot_path()):
                with open(_snapshot_path(), 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    S.menu = data.get('menu', [])
                    S.orders = data.get('orders', [])
                    S.archived_orders = data.get('archived_orders', [])
                    S.settings = data.get('settings', S.settings)
                    S.session = data.get('session', S.session)
                    S.printer = data.get('printer', S.printer)
                    S.sales_summary = data.get('sales_summary', S.sales_summary)
                    S.next_sku_main = data.get('next_sku_main', S.next_sku_main)
                    S.next_sku_side = data.get('next_sku_side', S.next_sku_side)
                    S.ensure_sales_summary_defaults()
                    S.ensure_session_defaults()
                    if not isinstance(S.next_sku_main, int) or S.next_sku_main <= 0:
                        S.next_sku_main = _infer_next_sku(S.menu, prefix='M', default=1)
                    if not isinstance(S.next_sku_side, int) or S.next_sku_side <= 0:
                        S.next_sku_side = _infer_next_sku(S.menu, prefix='S', default=1)
            # replay wal (best-effort; here we just append to archived log for trace)
            last_ts = None
            if os.path.exists(_wal_path()):
                with open(_wal_path(), 'r', encoding='utf-8') as f:
                    for ln in f:
                        try:
                            j = json.loads(ln)
                            last_ts = j.get('ts', last_ts)
                        except Exception:
                            continue
            return True, str(last_ts) if last_ts is not None else None
        except Exception as e:
            return False, str(e)

def get_pending_print_jobs() -> int:
    with _lock:
        # we don't have a separate queue here; just count orders not printed and not cancelled
        cnt = 0
        for o in S.orders:
            if not o.get('printed', False) and o.get('status','') != 'CANCELLED':
                cnt += 1
        return cnt

def enqueue_print(order: Dict):
    # mark printed False and leave it; actual print flow is handled by app.print_queue
    with _lock:
        order['printed'] = False

def order_to_json(order: Dict) -> Dict:
    return order

def find_order(orderNo: str) -> Optional[Dict]:
    with _lock:
        for o in S.orders:
            if o.get('orderNo') == orderNo:
                return o
        for a in S.archived_orders:
            if a.get('orderNo') == orderNo:
                return a
    return None

def archive_order_and_remove(orderNo: str) -> bool:
    with _lock:
        for i, o in enumerate(S.orders):
            if o.get('orderNo') == orderNo:
                archived = o.copy()
                archived['archivedAt'] = int(time.time())
                S.archived_orders.append(archived)
                del S.orders[i]
                return True
    return False

def archive_find_order(orderNo: str) -> Optional[Dict]:
    with _lock:
        for a in S.archived_orders:
            if a.get('orderNo') == orderNo:
                return a
    return None

def generate_order_no() -> str:
    # produce zero-padded 4-digit number like '0001'
    with _lock:
        seq = S.session.get('nextOrderSeq', 1)
        S.session['nextOrderSeq'] = seq + 1
        return f"{seq:04d}"

def generate_sku_main() -> str:
    with _lock:
        v = S.next_sku_main
        S.next_sku_main += 1
        return f"M{v:03d}"

def generate_sku_side() -> str:
    with _lock:
        v = S.next_sku_side
        S.next_sku_side += 1
        return f"S{v:03d}"


def _infer_next_sku(menu: List[Dict], prefix: str, default: int) -> int:
    highest = default - 1
    for item in menu:
        sku = str(item.get('sku') or '')
        if sku.startswith(prefix) and len(sku) > 1:
            tail = sku[1:]
            try:
                val = int(tail)
            except ValueError:
                continue
            if val > highest:
                highest = val
    return max(highest + 1, default)


def compute_order_total(order: Dict) -> int:
    total = 0
    for it in order.get('items', []):
        unit = it.get('unitPriceApplied')
        if unit is None:
            unit = it.get('unitPrice')
        if unit is None:
            unit = it.get('price')
        if unit is None:
            unit = it.get('price_single')
        if unit is None:
            unit = 0
        qty = it.get('qty', 1) or 1
        discount = it.get('discountValue') or it.get('discount', 0) or 0
        try:
            subtotal = int(unit) * int(qty) - int(discount)
        except Exception:
            try:
                subtotal = float(unit) * float(qty) - float(discount)
            except Exception:
                subtotal = 0
        total += int(subtotal)
    if total < 0:
        total = 0
    return total


def sales_summary_apply_order(order: Dict):
    with _lock:
        S.ensure_sales_summary_defaults()
        total = compute_order_total(order)
        status = order.get('status', '')
        if status == 'CANCELLED':
            sales_summary_apply_cancellation(order)
            return
        S.sales_summary['confirmedOrders'] = int(S.sales_summary.get('confirmedOrders', 0)) + 1
        S.sales_summary['revenue'] = int(S.sales_summary.get('revenue', 0)) + total
        S.sales_summary['lastUpdated'] = int(time.time())


def sales_summary_apply_cancellation(order: Dict):
    with _lock:
        S.ensure_sales_summary_defaults()
        total = compute_order_total(order)
        confirmed = int(S.sales_summary.get('confirmedOrders', 0))
        if confirmed > 0:
            S.sales_summary['confirmedOrders'] = confirmed - 1
        S.sales_summary['cancelledOrders'] = int(S.sales_summary.get('cancelledOrders', 0)) + 1
        revenue = int(S.sales_summary.get('revenue', 0)) - total
        if revenue < 0:
            revenue = 0
        S.sales_summary['revenue'] = revenue
        cancelled_amount = int(S.sales_summary.get('cancelledAmount', 0)) + total
        if cancelled_amount < 0:
            cancelled_amount = 0
        S.sales_summary['cancelledAmount'] = cancelled_amount
        S.sales_summary['lastUpdated'] = int(time.time())


def sales_summary_reset():
    with _lock:
        now = int(time.time())
        S.sales_summary = {
            'lastUpdated': now,
            'confirmedOrders': 0,
            'cancelledOrders': 0,
            'revenue': 0,
            'cancelledAmount': 0,
        }


def sales_summary_recalculate():
    with _lock:
        now = int(time.time())
        confirmed = 0
        cancelled = 0
        revenue = 0
        cancelled_amount = 0
        for order in S.orders + S.archived_orders:
            total = compute_order_total(order)
            if order.get('status') == 'CANCELLED':
                cancelled += 1
                cancelled_amount += total
            else:
                confirmed += 1
                revenue += total
        S.sales_summary = {
            'lastUpdated': now,
            'confirmedOrders': confirmed,
            'cancelledOrders': cancelled,
            'revenue': revenue,
            'cancelledAmount': cancelled_amount,
        }
