const state = {
    page: 'order',
    ws: null,
    online: false,
    data: null,
    menu: [],
    menuEtag: null,
    cart: [],
    settingsTab: 'main',
    callList: [],
    memory: null,
    archived: {
        sessionId: null,
        orders: [],
        loading: false,
        error: null,
        fetched: false
    },
    salesSummary: {
        data: null,
        sessionId: null,
        fetchedAt: null,
        loading: false,
        error: null
    }
};

let _reloadTimer = null;

function normalizeQrContentInput(value) {
    if (typeof value !== 'string') {
        return '';
    }

    let normalized = value.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    normalized = normalized.replace(/\t/g, ' ');
    normalized = normalized.replace(/[\uFF01-\uFF5E]/g, ch => String.fromCharCode(ch.charCodeAt(0) - 0xFEE0));
    normalized = normalized.replace(/\u3000/g, ' ');
    return normalized.trim();
}
function scheduleStateReload() {
    if (_reloadTimer) {
        return;
    }
    _reloadTimer = setTimeout(async () => {
        _reloadTimer = null;
        try {
            await loadStateData();
        } catch (error) {
            console.error('状態再取得スケジュール失敗:', error);
        }
    }, 400);
}

function renderMenu() {
    if (!state.data) {
        return;
    }
    state.data.menu = state.menu;
    if (state.page === 'order' || state.page === 'settings') {
        render();
    }
}

async function loadMenu(options = {}) {
    const { force = false } = options;
    try {
        const headers = {};
        if (!force && state.menuEtag) {
            headers['If-None-Match'] = state.menuEtag;
        }
        const response = await fetch('/api/menu', {
            method: 'GET',
            headers,
            cache: 'no-cache'
        });

        if (response.status === 304) {
            if (state.data) {
                state.data.menu = state.menu;
            }
            return false;
        }

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const etag = response.headers.get('ETag');
        if (etag) {
            state.menuEtag = etag;
        }

        const payload = await response.json();
        state.menu = Array.isArray(payload.menu) ? payload.menu : [];
        if (state.data) {
            state.data.menu = state.menu;
            renderMenu();
        }
        console.log('メニュー取得完了:', { count: state.menu.length, etag: state.menuEtag });
        return true;
    } catch (error) {
        console.error('loadMenu failed', error);
        return false;
    }
}

async function appInit() {
    try {
        try {
            await syncTimeOnce();
        } catch (error) {
            console.warn('初期時刻同期に失敗しましたが継続します:', error);
        }
        await loadMenu({ force: true });
        await loadStateData();
        fetchSalesSummary(true);
    } finally {
        connectWs();
    }
}

function getOrderFromState(orderNo) {
    if (!orderNo) {
        return null;
    }

    if (state.data && Array.isArray(state.data.orders)) {
        const activeOrder = state.data.orders.find(order => order && order.orderNo === orderNo);
        if (activeOrder) {
            return { order: activeOrder, source: 'active' };
        }
    }

    if (state.archived && Array.isArray(state.archived.orders)) {
        const archivedOrder = state.archived.orders.find(order => order && order.orderNo === orderNo);
        if (archivedOrder) {
            return { order: archivedOrder, source: 'archived' };
        }
    }

    return null;
}

let memoryMonitorTimer = null;

const app = document.getElementById('app');
const offlineModal = document.getElementById('offline-modal');
const reconnectBtn = document.getElementById('reconnect-btn');
const statusIndicator = document.getElementById('connection-status');

document.addEventListener('DOMContentLoaded', () => {
    console.log('KDS PWA init');

    if ('serviceWorker' in navigator) {
        navigator.serviceWorker.register('/sw.js')
            .then(reg => console.log('Service Worker registered', reg))
            .catch(err => console.error('Service Worker registration failed', err));
    }

    setupNavigation();
    setInterval(syncTimeOnce, 5 * 60 * 1000);

    appInit().catch(err => console.error('初期化処理に失敗しました:', err));

    reconnectBtn.addEventListener('click', connectWs);

    window.addEventListener('error', e => console.error('GLOBAL ERR', e.error || e.message));
    window.addEventListener('unhandledrejection', e => console.error('PROMISE REJECTION', e.reason));

    document.addEventListener('click', ev => {
        const btn = ev.target.closest("[data-action='confirm-order']");
        if (!btn) {
            return;
        }

        ev.preventDefault();

        if (btn.dataset.loading === '1') {
            return;
        }
        btn.dataset.loading = '1';

        // submitOrderのPromiseをグローバルで保持
        window.activeOrderPromise = submitOrder();
        window.activeOrderPromise.catch(console.error).finally(() => {
            delete btn.dataset.loading;
            window.activeOrderPromise = null;
        });
    });

    render();
    updateCurrentTime();
    setInterval(updateCurrentTime, 1000);
    setInterval(() => {
        if (state.page === 'call') {
            loadCallList();
        }
    }, 10000);
    loadCallList();
});

function updateCurrentTime() {
    const timeDiv = document.getElementById('current-time');
    if (timeDiv) {
        const now = new Date();
        timeDiv.textContent = now.toLocaleString('ja-JP', { 
            timeZone: 'Asia/Tokyo',
            year: 'numeric',
            month: '2-digit',
            day: '2-digit',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit'
        });
    }
    const callTimeDiv = document.querySelector('.call-time');
    if (callTimeDiv) {
        const now = new Date();
        callTimeDiv.textContent = now.toLocaleString('ja-JP', { 
            timeZone: 'Asia/Tokyo',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit'
        });
    }
}
/* File truncated when originally read; full file copied from repo to rpi/data/www/app.js */
