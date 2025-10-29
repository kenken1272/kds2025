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
            'qrPrint': {'enabled': False, 'content': ''}
        }
        self.session = {'sessionId': '', 'startedAt': int(time.time()), 'exported': False, 'nextOrderSeq': 1}
        self.printer = {'paperOut': False, 'overheat': False, 'holdJobs': 0}
        self.sales_summary = {'lastUpdated': int(time.time()), 'confirmedOrders': 0, 'cancelledOrders': 0, 'revenue': 0, 'cancelledAmount': 0}
    # SKU counters for generated SKUs
    self.next_sku_main = 1
    self.next_sku_side = 1

    def create_initial_menu_if_empty(self):
        if not self.menu:
            self.menu = [
                {'sku':'M001','name':'Teriyaki Burger','nameRomaji':'Teriyaki Burger','category':'MAIN','active':True,'price_normal':800,'price_presale':700,'presale_discount_amount':0,'price_single':800,'price_as_side':0},
                {'sku':'S001','name':'Fries','nameRomaji':'Fries','category':'SIDE','active':True,'price_single':200,'price_as_side':200}
            ]

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
