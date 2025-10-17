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
async function loadStateData(options = {}) {
    const { forceFull = false } = options;
    try {
        const preferLight = !forceFull && state.data !== null;
        const url = preferLight ? '/api/state?light=1' : '/api/state';
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const payload = await response.json();
        state.data = payload;
        if (Array.isArray(payload.menu)) {
            state.menu = payload.menu;
        }
        state.data.menu = state.menu;

        console.log('状態データ取得完了:', state.data);

        const needsSalesData = state.settingsTab === 'sales';
        const sessionId = state.data?.session?.sessionId || null;
        if (needsSalesData && sessionId) {
            await loadArchivedOrders(sessionId, true);
            ensureSalesSummary(true);
        } else {
            ensureSalesSummary(false);
        }

        render();
        updateConfirmOrderButton();
    } catch (error) {
        console.error('状態データ取得エラー:', error);
    }
}

async function loadArchivedOrders(sessionId, force = false) {
    if (!sessionId) {
        state.archived.sessionId = null;
        state.archived.orders = [];
        state.archived.loading = false;
        state.archived.error = null;
        state.archived.fetched = false;
        return;
    }

    if (state.archived.sessionId && state.archived.sessionId !== sessionId) {
        state.archived.fetched = false;
        state.archived.orders = [];
    }

    if (!force && state.archived.fetched && state.archived.sessionId === sessionId) {
        return;
    }
    if (state.archived.loading) {
        return;
    }

    state.archived.loading = true;
    state.archived.error = null;
    state.archived.sessionId = sessionId;
    if (state.settingsTab === 'sales') {
        render();
    }

    try {
        const response = await fetch(`/api/orders/archive?sessionId=${encodeURIComponent(sessionId)}`);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const data = await response.json();
        state.archived.sessionId = data.sessionId || sessionId;
        state.archived.orders = Array.isArray(data.orders) ? data.orders : [];
        state.archived.error = null;
        state.archived.fetched = true;
    } catch (error) {
        console.error('アーカイブ注文取得エラー:', error);
        state.archived.orders = [];
        state.archived.error = error.message || 'unknown';
        state.archived.fetched = false;
    } finally {
        state.archived.loading = false;
        if (state.settingsTab === 'sales') {
            render();
        }
        const widget = document.getElementById('completed-orders-widget');
        if (widget && widget.style.display !== 'none') {
            loadCompletedOrders();
        }
    }
}

function ensureArchivedOrders(force = false) {
    if (!state.data || !state.data.session) {
        return;
    }
    const sessionId = state.data.session.sessionId;
    if (!sessionId) {
        return;
    }
    if (state.archived.loading) {
        return;
    }
    if (!force && state.archived.fetched && state.archived.sessionId === sessionId) {
        return;
    }
    loadArchivedOrders(sessionId, force);
}

function ensureSalesSummary(force = false) {
    if (!state.data || !state.data.session) {
        return;
    }

    const sessionId = state.data.session.sessionId;
    if (!sessionId) {
        return;
    }

    if (state.salesSummary.loading) {
        return;
    }

    const hasCurrentSummary = state.salesSummary.data && state.salesSummary.sessionId === sessionId;
    if (!force && hasCurrentSummary) {
        return;
    }

    fetchSalesSummary(force);
}

async function fetchSalesSummary(force = false) {
    if (state.salesSummary.loading) {
        return;
    }

    state.salesSummary.loading = true;
    state.salesSummary.error = null;

    if (state.settingsTab === 'sales') {
        render();
    }

    try {
        const params = [];
        if (force) {
            params.push('rebuild=1');
        }
        params.push(`ts=${Date.now()}`);
        const url = `/api/sales/summary?${params.join('&')}`;
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const data = await response.json();
        state.salesSummary.data = data;
        if (data && data.sessionId) {
            state.salesSummary.sessionId = data.sessionId;
        } else if (state.data && state.data.session) {
            state.salesSummary.sessionId = state.data.session.sessionId;
        } else {
            state.salesSummary.sessionId = null;
        }
        state.salesSummary.fetchedAt = Date.now();
    } catch (error) {
        console.error('売上サマリ取得エラー:', error);
        state.salesSummary.error = error && error.message ? error.message : 'unknown';
    } finally {
        state.salesSummary.loading = false;
        if (state.settingsTab === 'sales') {
            render();
        }
    }
}
async function syncTimeOnce() {
    const now = new Date();
    const epoch = Math.floor(now.getTime() / 1000);
    
    console.log('時刻同期開始:', {
        localTime: now.toLocaleString('ja-JP', { timeZone: 'Asia/Tokyo' }),
        epoch: epoch,
        iso: now.toISOString()
    });
    
    try {
        const response = await fetch('/api/time/set', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ epoch })
        });
        
        if (response.ok) {
            console.log('時刻同期完了:', now.toLocaleString('ja-JP', { timeZone: 'Asia/Tokyo' }));
        } else {
            console.error('時刻同期失敗: HTTP', response.status);
        }
    } catch (e) {
        console.error('時刻同期エラー:', e);
    }
}
function setupNavigation() {
    const navBtns = document.querySelectorAll('.nav-btn');
    
    navBtns.forEach(btn => {
        btn.addEventListener('click', (e) => {
            const page = e.target.dataset.page;
            if (page) {            
                navBtns.forEach(b => b.classList.remove('active'));
                e.target.classList.add('active');
                state.page = page;
                if (page === 'call') {
                    loadCallList().then(() => render());
                } else {
                    render();
                }
            }
        });
    });
    document.querySelector(`[data-page="${state.page}"]`).classList.add('active');
}
function navigateTo(page) {
    state.page = page;
    const navBtns = document.querySelectorAll('.nav-btn');
    navBtns.forEach(btn => {
        if (btn.dataset.page === page) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    });
    if (page === 'call') {
        loadCallList().then(() => render());
    } else {
        render();
    }
}
function connectWs() {
    const wsUrl = `ws://${location.host}/ws`;
    console.log('WebSocket接続試行:', wsUrl);
    
    state.ws = new WebSocket(wsUrl);
    
    state.ws.onopen = () => {
        console.log('WebSocket接続成功');
        updateOnlineStatus(true);
    };
    
    state.ws.onclose = () => {
        console.log('WebSocket接続切断');
        updateOnlineStatus(false);
        setTimeout(connectWs, 3000);
    };
    
    state.ws.onerror = (error) => {
        console.error('WebSocket エラー:', error);
        updateOnlineStatus(false);
    };
    
    state.ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            console.log('WebSocket メッセージ受信:', data);
            
            if (data.type === 'hello') {
                console.log('サーバーから挨拶:', data.msg);
            } else if (data.type === 'sync.snapshot') {
                loadMenu();
                scheduleStateReload();
            } else if (data.type === 'system.reset') {
                loadMenu({ force: true });
                scheduleStateReload();
            } else if (data.type === 'order.created' || data.type === 'order.updated') {
                scheduleStateReload();
            } else if (data.type === 'printer.status') {
                if (state.data) {
                    state.data.printer.paperOut = data.paperOut !== undefined ? data.paperOut : state.data.printer.paperOut;
                    state.data.printer.holdJobs = data.holdJobs !== undefined ? data.holdJobs : state.data.printer.holdJobs;
                    render();
                    updateConfirmOrderButton();
                }
            } else if (data.type === 'order.cooked') {
                const exists = state.callList.find(item => item.orderNo === data.orderNo);
                if (!exists) {
                    state.callList.push({ orderNo: data.orderNo, ts: Date.now() / 1000 });
                    console.log('呼び出しリストに追加:', data.orderNo);
                }
                if (state.page === 'call') {
                    updateCallScreen();
                } else {
                    scheduleStateReload();
                }
            } else if (data.type === 'order.picked') {
                const beforeLength = state.callList.length;
                state.callList = state.callList.filter(item => item.orderNo !== data.orderNo);
                if (beforeLength !== state.callList.length) {
                    console.log('呼び出しリストから削除:', data.orderNo);
                }
                if (state.page === 'call') {
                    updateCallScreen();
                } else {
                    scheduleStateReload();
                }
            }
            
        } catch (err) {
            console.error('メッセージ解析エラー:', err);
        }
    };
}
function updateOnlineStatus(online) {
    state.online = online;
    
    statusIndicator.className = `status-indicator ${online ? 'online' : 'offline'}`;
    
    if (online) {
        offlineModal.classList.add('hidden');
    } else {
        offlineModal.classList.remove('hidden');
    }
}

function updatePaperOutModal() {
    const modal = document.getElementById('paper-out-modal');
    if (state.data && state.data.printer.paperOut) {
        if (!modal) {
            const modalHtml = `
                <div id="paper-out-modal" class="modal">
                    <div class="modal-content">
                        <h3>⚠️ プリンタ用紙切れ</h3>
                        <p>用紙を交換してください。注文受付は一時停止しています。</p>
                        <button id="paper-replaced-btn" class="btn btn-primary">用紙交換済み</button>
                    </div>
                </div>
            `;
            document.body.insertAdjacentHTML('beforeend', modalHtml);
            
            document.getElementById('paper-replaced-btn').addEventListener('click', async () => {
                try {
                    await fetch('/api/printer/paper-replaced', { method: 'POST' });
                    document.getElementById('paper-out-modal').remove();
                } catch (error) {
                    console.error('用紙交換通知エラー:', error);
                }
            });
        }
    } else if (modal) {
        modal.remove();
    }
}
function render() {
    let content = '';
    
    switch (state.page) {
        case 'order':
            content = renderOrderPage();
            break;
        case 'kitchen':
            content = renderKitchenPage();
            break;
        case 'pickup':
            content = renderPickupPage();
            break;
        case 'settings':
            content = renderSettingsPage();
            break;
        case 'export':
            content = renderExportPage();
            break;
        case 'call':
            content = renderCallPage();
            break;
        default:
            content = '<div class="card"><h2>ページが見つかりません</h2></div>';
    }
    
    app.innerHTML = content;
    const nav = document.querySelector('nav.nav');
    if (nav) {
        if (state.page === 'call') {
            nav.style.display = 'none';
        } else {
            nav.style.display = 'flex';
        }
    }

    updatePaperOutModal();
    setupPageEvents();
}

function renderOrderPage() {
    if (!state.data) {
        return '<div class="card"><h2>📱 注文受付</h2><p>データ読込中...</p></div>';
    }
    
    const mainItems = state.data.menu.filter(item => item.category === 'MAIN' && item.active);
    const sideItems = state.data.menu.filter(item => item.category === 'SIDE' && item.active);
    const cookingOrders = state.data.orders.filter(order => order.status === 'COOKING');
    const paperWarning = state.data.printer.paperOut ? 
        '<div class="card" style="border-left-color: #dc3545;"><h3>⚠️ 注文受付停止中</h3><p>プリンタ用紙切れのため、注文を受け付けできません。</p></div>' : '';
    
    return `
        ${paperWarning}
        
        <div class="card">
            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
                <h2>新規注文</h2>
                <button class="btn btn-info" onclick="toggleCompletedOrders()" id="toggle-completed-btn">
                    📋 キャンセル・再印刷
                </button>
            </div>
            
            <!-- 注文済み一覧ウィジェット -->
            <div id="completed-orders-widget" class="completed-orders-widget" style="display: none;">
                <h3>📋 注文済み一覧</h3>
                <div id="completed-orders-list" class="completed-orders-list">
                    <!-- 動的に生成 -->
                </div>
            </div>
            
            <!-- メイン選択 -->
            <h3>メイン商品</h3>
            <div class="grid">
                ${mainItems.map(item => {
                    const normalPrice = item.price_normal;
                    const presalePrice = item.price_normal + item.presale_discount_amount;
                    const hasPresale = state.data.settings.presaleEnabled && item.presale_discount_amount < 0;
                    const hasSides = sideItems.length > 0;
                    
                    return `
                    <div class="card">
                        <h4>${item.name}</h4>
                        <p style="margin: 5px 0;">通常: ${normalPrice}円</p>
                        ${hasPresale ? `<p style="margin: 5px 0;">前売: ${presalePrice}円</p>` : ''}
                        
                        <!-- 単品ボタン -->
                        <button class="btn btn-primary" onclick="addMainSingle('${item.sku}', 'normal')" 
                                ${state.data.printer.paperOut ? 'disabled' : ''}
                                style="width: 100%; margin-bottom: 5px;">
                            通常
                        </button>
                        ${hasPresale ? `
                        <button class="btn btn-success" onclick="addMainSingle('${item.sku}', 'presale')" 
                                ${state.data.printer.paperOut ? 'disabled' : ''}
                                style="width: 100%; margin-bottom: 5px;">
                            前売り
                        </button>` : ''}
                        
                        <!-- セットボタン -->
                        ${hasSides ? `
                        <button class="btn btn-warning" onclick="showSideSelectModal('${item.sku}', 'normal')" 
                                ${state.data.printer.paperOut ? 'disabled' : ''}
                                style="width: 100%; margin-bottom: 5px;">
                            セット（通常）
                        </button>
                        ${hasPresale ? `
                        <button class="btn btn-info" onclick="showSideSelectModal('${item.sku}', 'presale')" 
                                ${state.data.printer.paperOut ? 'disabled' : ''}
                                style="width: 100%; margin-bottom: 5px;">
                            セット（前売り）
                        </button>` : ''}
                        ` : '<p style="color: #999; font-size: 0.9em;">※サイド商品なし</p>'}
                    </div>
                `}).join('')}
            </div>
            
            <!-- サイド単品 -->
            <h3>サイド単品</h3>
            <div class="grid">
                ${sideItems.map(item => `
                    <div class="card">
                        <h4>${item.name}</h4>
                        <p>単品: ${item.price_single}円</p>
                        <button class="btn btn-secondary" onclick="addToCart('SIDE_SINGLE', '${item.sku}')" ${state.data.printer.paperOut ? 'disabled' : ''}>
                            追加
                        </button>
                    </div>
                `).join('')}
            </div>
            
            <!-- カート -->
            <div class="card">
                <h3>注文カート</h3>
                <div id="cart-items"></div>
                <div style="margin-top: 15px;">
                    <button id="confirm-order-btn" class="btn btn-success btn-large" 
                            data-action="confirm-order" 
                            type="button" 
                            onclick="handleConfirmOrder(event)"
                            ${state.cart.length === 0 || state.data.printer.paperOut ? 'disabled' : ''} 
                            style="font-size: 1.5em; padding: 15px 30px; width: 100%; margin-bottom: 10px;">
                        📝 注文確定
                    </button>
                    <button class="btn btn-secondary" onclick="clearCart()" style="width: 100%;">
                        🗑️ カートクリア
                    </button>
                    <div style="margin-top: 10px; padding: 8px; background: #f8f9fa; border-radius: 5px; font-size: 0.85em; color: #666; text-align: center;">
                        💡 注文確定ボタンをクリックすると即座に注文が送信されます（画面遷移不要）
                    </div>
                </div>
            </div>
        </div>
        

    `;
}

function renderKitchenPage() {
    if (!state.data) {
        return '<div class="card"><h2>👨‍🍳 キッチン表示</h2><p>データ読込中...</p></div>';
    }
    
    const cookingOrders = state.data.orders.filter(order => order.status === 'COOKING');
    
    return `
        <div class="kitchen-container">
            <h2 style="text-align: center; margin-bottom: 20px;">調理一覧 (${cookingOrders.length}件)</h2>
            <div class="kitchen-grid">
                ${cookingOrders.map(order => {
                    const elapsed = order.ts && order.ts > 946684800 ? Math.floor((Date.now() / 1000 - order.ts) / 60) : 0;
                    const elapsedColor = elapsed > 10 ? '#dc3545' : elapsed > 5 ? '#ffc107' : '#28a745';
                    
                    return `
                        <div class="kitchen-card" onclick="showOrderDetail('${order.orderNo}')">
                            <div class="kitchen-header">
                                <h1 style="font-size: 1.8em; margin: 0; color: #333;">注文 #${order.orderNo}</h1>
                                <div class="elapsed-time" style="color: ${elapsedColor}; font-weight: bold; font-size: 1em;">
                                    経過: ${elapsed}分
                                </div>
                            </div>
                            <div class="kitchen-items">
                                ${order.items.map(item => `
                                    <div class="kitchen-item">
                                        <div class="item-name" style="font-size: 1.2em; font-weight: bold;">${item.name}</div>
                                        <div class="item-qty" style="font-size: 1.4em; color: #007bff; font-weight: bold;">
                                            数量: ${item.qty}個
                                        </div>
                                        ${item.priceMode === 'presale' ? '<div class="presale-badge">前売</div>' : ''}
                                    </div>
                                `).join('')}
                            </div>
                            <div class="kitchen-footer">
                                <div class="kitchen-note" style="color: #666; font-size: 0.8em; text-align: center;">
                                    調理完了後は品出し画面で操作してください
                                </div>
                            </div>
                        </div>
                    `;
                }).join('')}
            </div>
            ${cookingOrders.length === 0 ? 
                '<div class="no-orders" style="text-align: center; font-size: 1.5em; color: #666; margin-top: 50px;">調理待ちの注文はありません</div>' : 
                ''}
        </div>
    `;
}

function renderPickupPage() {
    if (!state.data) {
        return '<div class="card"><h2>📦 品出し管理</h2><p>データ読込中...</p></div>';
    }
    const pickupOrders = state.data.orders
        .filter(order => order.status === 'COOKING' || order.status === 'DONE')
        .sort((a, b) => b.ts - a.ts)
        .slice(0, 30);
    
    return `
        <div class="card">
            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px;">
                <h2>品出し画面 (${pickupOrders.length}件)</h2>
            </div>
            <div class="grid">
                ${pickupOrders.map(order => {
                    const elapsed = order.ts && order.ts > 946684800 ? Math.floor((Date.now() / 1000 - order.ts) / 60) : 0;
                    const elapsedColor = elapsed > 15 ? '#dc3545' : elapsed > 10 ? '#ffc107' : '#28a745';
                    const itemsSummary = order.items.slice(0, 3).map(item => 
                        `${item.name}(${item.unitPriceApplied || item.unitPrice || 0}円)`
                    ).join(', ') + (order.items.length > 3 ? '...' : '');
                    
                    const isCooking = order.status === 'COOKING';
                    const statusText = isCooking ? '調理中' : '調理完了';
                    const statusColor = isCooking ? '#ffc107' : '#28a745';
                    const actionText = isCooking ? '品出し完了' : '品出し完了';
                    
                    return `
                        <div class="card order-card pickup-card" 
                             data-order-no="${order.orderNo}"
                             style="border-left-color: ${statusColor}; background-color: #f8f9fa; cursor: pointer;">
                            <div class="pickup-header">
                                <h3># ${order.orderNo}</h3>
                                <span class="badge" style="background-color: ${statusColor};">${statusText}</span>
                            </div>
                            <div class="pickup-timing" style="color: ${elapsedColor}; font-weight: bold;">
                                ${isCooking ? '調理開始から' : '完了から'}${elapsed}分経過
                            </div>
                            <div class="pickup-items">
                                <strong>${itemsSummary}</strong>
                            </div>
                            <div class="pickup-actions" style="text-align: center; margin-top: 15px;">
                                <button class="btn btn-primary btn-large" 
                                        onclick="showOrderDetail('${order.orderNo}')" 
                                        style="font-size: 1.4em; padding: 15px 30px; width: 100%;">
                                    📋 状態を変更する
                                </button>
                                <small style="display: block; margin-top: 8px; color: #666; font-size: 0.95em;">
                                    👆 クリックして調理済み・品出し済みにできます
                                </small>
                            </div>
                        </div>
                    `;
                }).join('')}
            </div>
            ${pickupOrders.length === 0 ? '<p>品出し待ちの注文はありません</p>' : ''}
        </div>
    `;
}

function renderSettingsPage() {
    if (!state.data) {
        return '<div class="card"><h2>⚙️ システム設定</h2><p>データ読込中...</p></div>';
    }
    const tabNav = `
        <nav class="nav" style="margin-bottom: 20px;">
            <button class="nav-btn ${state.settingsTab === 'main' ? 'active' : ''}" onclick="switchSettingsTab('main')">メイン商品</button>
            <button class="nav-btn ${state.settingsTab === 'side' ? 'active' : ''}" onclick="switchSettingsTab('side')">サイド商品</button>
            <button class="nav-btn ${state.settingsTab === 'system' ? 'active' : ''}" onclick="switchSettingsTab('system')">システム設定</button>
            <button class="nav-btn ${state.settingsTab === 'sales' ? 'active' : ''}" onclick="switchSettingsTab('sales')">売上確認</button>
            <button class="nav-btn ${state.settingsTab === 'chinchiro' ? 'active' : ''}" onclick="switchSettingsTab('chinchiro')">ちんちろ</button>
            <button class="nav-btn ${state.settingsTab === 'qrprint' ? 'active' : ''}" onclick="switchSettingsTab('qrprint')">プリント設定</button>
        </nav>
    `;
    
    let tabContent = '';
    
    if (state.settingsTab === 'main') {
        const mainItems = state.data.menu.filter(item => item.category === 'MAIN');
        tabContent = `
            <div class="card">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
                    <h3>メイン商品管理</h3>
                    <button class="btn btn-success" onclick="addNewMainItem()">➕ 新規追加</button>
                </div>
                <div class="menu-items-grid" id="main-items">
                    ${mainItems.map((item, idx) => `
                        <div class="menu-item-card" data-sku="${item.sku}" data-idx="${idx}">
                            <div class="menu-item-header">
                                <div class="drag-handle">⋮⋮</div>
                                <label class="toggle-switch">
                                    <input type="checkbox" ${item.active ? 'checked' : ''} onchange="toggleMainItemActive('${item.sku}', this.checked)">
                                    <span class="toggle-slider"></span>
                                </label>
                            </div>
                            <div class="menu-item-body">
                                <div class="form-group">
                                    <label>商品名</label>
                                    <input type="text" class="form-control" value="${item.name}" 
                                           onchange="updateMainItem('${item.sku}', 'name', this.value)">
                                </div>
                                <div class="form-group">
                                    <label>商品名（ローマ字）</label>
                                    <input type="text" class="form-control" value="${item.nameRomaji || ''}" 
                                           onchange="updateMainItem('${item.sku}', 'nameRomaji', this.value)">
                                </div>
                                <div class="form-row">
                                    <div class="form-group">
                                        <label>通常価格</label>
                                        <input type="number" class="form-control" value="${item.price_normal}" 
                                               onchange="updateMainItem('${item.sku}', 'price_normal', parseInt(this.value))">
                                    </div>
                                    <div class="form-group">
                                        <label>前売割引額</label>
                                        <input type="number" class="form-control" value="${item.presale_discount_amount}" 
                                               onchange="updateMainItem('${item.sku}', 'presale_discount_amount', parseInt(this.value))">
                                    </div>
                                </div>
                                <small class="text-muted">SKU: ${item.sku}</small>
                            </div>
                        </div>
                    `).join('')}
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'side') {
        const sideItems = state.data.menu.filter(item => item.category === 'SIDE');
        tabContent = `
            <div class="card">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
                    <h3>サイド商品管理</h3>
                    <button class="btn btn-success" onclick="addNewSideItem()">➕ 新規追加</button>
                </div>
                <div class="menu-items-grid" id="side-items">
                    ${sideItems.map((item, idx) => `
                        <div class="menu-item-card" data-sku="${item.sku}" data-idx="${idx}">
                            <div class="menu-item-header">
                                <div class="drag-handle">⋮⋮</div>
                                <label class="toggle-switch">
                                    <input type="checkbox" ${item.active ? 'checked' : ''} onchange="toggleSideItemActive('${item.sku}', this.checked)">
                                    <span class="toggle-slider"></span>
                                </label>
                            </div>
                            <div class="menu-item-body">
                                <div class="form-group">
                                    <label>商品名</label>
                                    <input type="text" class="form-control" value="${item.name}" 
                                           onchange="updateSideItem('${item.sku}', 'name', this.value)">
                                </div>
                                <div class="form-group">
                                    <label>商品名（ローマ字）</label>
                                    <input type="text" class="form-control" value="${item.nameRomaji || ''}" 
                                           onchange="updateSideItem('${item.sku}', 'nameRomaji', this.value)">
                                </div>
                                <div class="form-row">
                                    <div class="form-group">
                                        <label>単品価格</label>
                                        <input type="number" class="form-control" value="${item.price_single}" 
                                               onchange="updateSideItem('${item.sku}', 'price_single', parseInt(this.value))">
                                    </div>
                                    <div class="form-group">
                                        <label>セット時価格</label>
                                        <input type="number" class="form-control" value="${item.price_as_side}" 
                                               onchange="updateSideItem('${item.sku}', 'price_as_side', parseInt(this.value))">
                                    </div>
                                </div>
                                <small class="text-muted">SKU: ${item.sku}</small>
                            </div>
                        </div>
                    `).join('')}
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'system') {
        tabContent = `
            <div class="card">
                <div class="settings-header">
                    <div>
                        <h3>システム設定</h3>
                        <p class="settings-subtitle">KDSの基本設定を管理します</p>
                    </div>
                    <div class="settings-actions">
                        <button class="btn btn-primary" onclick="saveSystemSettings()">💾 システム設定を保存</button>
                    </div>
                </div>

                <div class="settings-panel">
                    <section class="settings-section">
                        <h4>前売り機能</h4>
                        <p class="settings-note">無効にすると注文画面で前売りボタンが非表示になります</p>
                        <label class="settings-toggle">
                            <input type="checkbox" ${state.data.settings.presaleEnabled ? 'checked' : ''} id="presale-enabled">
                            <span>前売り機能を有効にする</span>
                        </label>
                    </section>

                    <section class="settings-section">
                        <h4>店舗情報</h4>
                        <div class="settings-field">
                            <label for="store-name">店舗名</label>
                            <input type="text" value="${state.data.settings.store.name}" id="store-name">
                        </div>
                        <div class="settings-field">
                            <label for="store-name-romaji">店舗名（レシート印刷用ローマ字）</label>
                            <input type="text" value="${state.data.settings.store.nameRomaji || 'KDS BURGER'}" id="store-name-romaji">
                            <small>レシート印刷時に使用される英語表記です</small>
                        </div>
                        <div class="settings-field">
                            <label for="register-id">レジスターID</label>
                            <input type="text" value="${state.data.settings.store.registerId}" id="register-id">
                        </div>
                    </section>

                    <section class="settings-section">
                        <h4>注文番号設定</h4>
                        <div class="settings-inline">
                            <div class="settings-field">
                                <label for="numbering-min">最小番号</label>
                                <input type="number" value="${state.data.settings.numbering.min}" id="numbering-min" min="1" max="9999">
                            </div>
                            <div class="settings-field">
                                <label for="numbering-max">最大番号</label>
                                <input type="number" value="${state.data.settings.numbering.max}" id="numbering-max" min="1" max="9999">
                            </div>
                        </div>
                    </section>

                    <section class="settings-section settings-danger">
                        <h4>⚠️ 初期化</h4>
                        <p class="settings-note">システムを完全に初期化します。全ての注文データ、注文番号カウンタ、設定が削除されます。</p>
                        <button class="btn btn-danger" onclick="resetSystem()">🔄 システム完全初期化</button>
                    </section>
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'sales') {
        if (state.salesSummary.loading) {
            tabContent = `
                <div class="card">
                    <h3>売上確認</h3>
                    <p>売上サマリを読込中...</p>
                </div>
            `;
        } else if (state.salesSummary.error) {
            tabContent = `
                <div class="card">
                    <h3>売上確認</h3>
                    <p style="color:#d32f2f;">売上サマリの取得に失敗しました: ${state.salesSummary.error}</p>
                    <button class="btn btn-primary" onclick="fetchSalesSummary(true)">再試行</button>
                </div>
            `;
        } else if (state.salesSummary.data) {
            const summary = state.salesSummary.data;
            const updatedAt = summary.updatedAt ? new Date(summary.updatedAt * 1000).toLocaleString('ja-JP') : '不明';
            const confirmed = summary.confirmedOrders || 0;
            const cancelled = summary.cancelledOrders || 0;
            const totalOrders = summary.totalOrders != null ? summary.totalOrders : confirmed + cancelled;
            const netSales = summary.netSales || 0;
            const grossSales = summary.grossSales != null ? summary.grossSales : netSales + (summary.cancelledAmount || 0);
            const cancelledAmount = summary.cancelledAmount || 0;
            const averageOrder = confirmed > 0 ? Math.round(netSales / confirmed) : 0;
            const cancelRate = totalOrders > 0 ? ((cancelled / totalOrders) * 100).toFixed(1) : '0.0';
            const adjustment = grossSales - netSales;

            tabContent = `
                <div class="card">
                    <div style="display: flex; justify-content: space-between; align-items: center; gap: 12px;">
                        <h3 style="margin: 0;">売上確認</h3>
                        <button class="btn btn-primary" onclick="refreshSalesStats()" style="font-size: 0.95em; padding: 8px 14px;">
                            🔄 売上データ更新
                        </button>
                    </div>

                    <div class="sales-overview" style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0;">
                        <div class="stat-card" style="background: #e3f2fd; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #1976d2;">総注文数</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #1976d2;">${totalOrders.toLocaleString()}</div>
                            <small style="color: #666;">件 (確定 ${confirmed.toLocaleString()} / キャンセル ${cancelled.toLocaleString()})</small>
                        </div>
                        <div class="stat-card" style="background: #e8f5e8; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #388e3c;">総売上</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #388e3c;">¥${netSales.toLocaleString()}</div>
                            <small style="color: #666;">円 (純売上)</small>
                        </div>
                        <div class="stat-card" style="background: #fff3e0; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #f57c00;">平均単価</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #f57c00;">¥${averageOrder.toLocaleString()}</div>
                            <small style="color: #666;">円 / 注文</small>
                        </div>
                        <div class="stat-card" style="background: #fce4ec; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #c2185b;">キャンセル額</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #c2185b;">¥${cancelledAmount.toLocaleString()}</div>
                            <small style="color: #666;">累計キャンセル金額</small>
                        </div>
                        <div class="stat-card" style="background: #ede7f6; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #5e35b1;">キャンセル率</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #5e35b1;">${cancelRate}%</div>
                            <small style="color: #666;">キャンセル ${cancelled.toLocaleString()} 件</small>
                        </div>
                        ${adjustment !== 0 ? `
                        <div class="stat-card" style="background: #e0f7fa; padding: 15px; border-radius: 8px; text-align: center;">
                            <h4 style="margin: 0 0 10px 0; color: #00796b;">調整差額</h4>
                            <div style="font-size: 2em; font-weight: bold; color: #00796b;">¥${adjustment.toLocaleString()}</div>
                            <small style="color: #666;">総売上 (毛) ¥${grossSales.toLocaleString()}</small>
                        </div>
                        ` : ''}
                    </div>

                    <div style="margin-top: 30px; padding: 12px; background: #f8f9fa; border-radius: 6px; font-size: 0.9em; color: #555;">
                        <strong>更新時刻:</strong> ${updatedAt}
                    </div>
                </div>
            `;
        } else {
            tabContent = `
                <div class="card">
                    <h3>売上確認</h3>
                    <p>売上サマリを表示できませんでした。もう一度読み込んでください。</p>
                    <button class="btn btn-primary" onclick="fetchSalesSummary(true)">再読込</button>
                </div>
            `;
        }
    } else if (state.settingsTab === 'chinchiro') {
        tabContent = `
            <div class="card">
                <div class="settings-header">
                    <div>
                        <h3>🎲 ちんちろ設定</h3>
                        <p class="settings-subtitle">セット商品の倍率と丸め方法を調整します</p>
                    </div>
                </div>

                <div class="settings-panel">
                    <section class="settings-section">
                        <h4>利用可否</h4>
                        <label class="settings-toggle">
                            <input type="checkbox" ${state.data.settings.chinchiro.enabled ? 'checked' : ''} id="chinchiro-enabled">
                            <span>ちんちろ機能を有効にする</span>
                        </label>
                        <p class="settings-note">有効にすると、注文画面でセット商品の価格倍率を選択できます。</p>
                    </section>

                    <section class="settings-section">
                        <h4>倍率設定</h4>
                        <p class="settings-note">カンマ区切りで倍率を指定（例: 0,0.5,1,2,3）</p>
                        <div class="settings-field">
                            <label for="chinchiro-multipliers">倍率リスト</label>
                            <input type="text" value="${state.data.settings.chinchiro.multipliers.join(',')}" id="chinchiro-multipliers">
                        </div>
                        <div class="settings-field">
                            <strong>倍率の意味</strong>
                            <ul class="settings-list">
                                <li><code>0</code> = 無料（ピンゾロ）</li>
                                <li><code>0.5</code> = 半額</li>
                                <li><code>1</code> = 通常価格（変更なし）</li>
                                <li><code>2</code> = 2倍</li>
                                <li><code>3</code> = 3倍</li>
                            </ul>
                        </div>
                    </section>

                    <section class="settings-section">
                        <h4>丸め方式</h4>
                        <p class="settings-note">調整額に小数が出た場合の処理方法</p>
                        <div class="settings-field">
                            <label for="chinchiro-rounding">丸め方法</label>
                            <select id="chinchiro-rounding">
                                <option value="round" ${state.data.settings.chinchiro.rounding === 'round' ? 'selected' : ''}>四捨五入</option>
                                <option value="floor" ${state.data.settings.chinchiro.rounding === 'floor' ? 'selected' : ''}>切り捨て（お客様有利）</option>
                                <option value="ceil" ${state.data.settings.chinchiro.rounding === 'ceil' ? 'selected' : ''}>切り上げ（店舗有利）</option>
                            </select>
                        </div>
                    </section>
                </div>

                <div class="settings-actions">
                    <button class="btn btn-primary btn-large" onclick="saveChinchoiroSettings()">💾 設定を保存</button>
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'qrprint') {
        tabContent = `
            <div class="card">
                <div class="settings-header">
                    <div>
                        <h3>🖨️ プリント設定</h3>
                        <p class="settings-subtitle">レシート印刷時のQRコード設定</p>
                    </div>
                </div>

                <div class="settings-panel">
                    <section class="settings-section">
                        <h4>QRコード印刷</h4>
                        <label class="settings-toggle">
                            <input type="checkbox" ${state.data.settings.qrPrint.enabled ? 'checked' : ''} id="qrprint-enabled">
                            <span>QRコード印刷を有効にする</span>
                        </label>
                        <p class="settings-note">有効にすると、レシートの最後にQRコードが印刷されます。</p>
                    </section>

                    <section class="settings-section">
                        <h4>QRコード内容</h4>
                        <p class="settings-note">URL、メッセージ等を入力してください</p>
                        <div class="settings-field">
                            <label for="qrprint-content">印刷する内容</label>
                            <textarea id="qrprint-content" placeholder="例: https://example.com&#10;またはメッセージテキスト">${state.data.settings.qrPrint.content || ''}</textarea>
                        </div>
                        <div class="settings-field">
                            <strong>使用例</strong>
                            <ul class="settings-list">
                                <li>店舗ウェブサイトURL</li>
                                <li>アンケートフォーム</li>
                                <li>SNSアカウント</li>
                                <li>クーポンコード</li>
                                <li>お礼メッセージ</li>
                            </ul>
                        </div>
                    </section>
                </div>

                <div class="settings-actions">
                    <button class="btn btn-primary btn-large" onclick="saveQrPrintSettings()">💾 設定を保存</button>
                </div>
            </div>
        `;
    }
    
    return tabNav + tabContent;
}

function renderCallPage() {
    const hasOrders = state.callList.length > 0;
    const items = hasOrders ? state.callList.map(item => `
        <div class="call-item" data-order="${item.orderNo}">
            <div class="call-number">${item.orderNo}</div>
            <div class="call-label">番</div>
        </div>
    `).join('') : '';
    
    return `
        <div class="call-screen">
            ${hasOrders ? `
                <div class="call-header">
                    <h1 onclick="navigateTo('order')" style="cursor: pointer; user-select: none;">お呼び出し</h1>
                </div>
                <div class="call-grid" id="call-grid">
                    ${items}
                </div>
            ` : `
                <div class="call-empty" id="call-empty">
                    <h1 onclick="navigateTo('order')" style="cursor: pointer; user-select: none;">お待ちください</h1>
                    <p>現在、呼び出し中の注文はありません</p>
                </div>
            `}
            <div class="call-footer" style="position: fixed; bottom: 0; left: 0; right: 0; display: flex; justify-content: space-between; align-items: center; padding: 15px 20px; background: rgba(0,0,0,0.8);">
                <div class="call-time" style="color: white; font-size: 1.2em;"></div>
                <button onclick="navigateTo('order')" style="background: rgba(100, 149, 237, 0.3); border: none; color: transparent; width: 60px; height: 40px; border-radius: 5px; cursor: pointer;">nav</button>
            </div>
        </div>
    `;
}

function updateCallScreen() {
    const hasOrders = state.callList.length > 0;
    const callScreen = document.querySelector('.call-screen');
    
    if (!callScreen) {
        return;
    }
    
    const callGrid = document.getElementById('call-grid');
    const callEmpty = document.getElementById('call-empty');
    
    if (hasOrders) {
        const items = state.callList.map(item => `
            <div class="call-item" data-order="${item.orderNo}">
                <div class="call-number">${item.orderNo}</div>
                <div class="call-label">番</div>
            </div>
        `).join('');
        
        if (callGrid) {
            callGrid.innerHTML = items;
        } else if (callEmpty) {
            callEmpty.outerHTML = `
                <div class="call-header">
                    <h1 onclick="navigateTo('order')" style="cursor: pointer; user-select: none;">お呼び出し</h1>
                </div>
                <div class="call-grid" id="call-grid">
                    ${items}
                </div>
            `;
        }
    } else {
        if (callGrid) {
            const header = callScreen.querySelector('.call-header');
            if (header) header.remove();
            callGrid.outerHTML = `
                <div class="call-empty" id="call-empty">
                    <h1 onclick="navigateTo('order')" style="cursor: pointer; user-select: none;">お待ちください</h1>
                    <p>現在、呼び出し中の注文はありません</p>
                </div>
            `;
        } else if (callEmpty) {
        }
    }
}

function renderExportPage() {
    return `
        <div class="card">
            <h2>データエクスポート</h2>
            <p style="color: #666; margin-bottom: 20px;">売上データのエクスポートとバックアップから復旧ができます</p>
            
            <div style="display: flex; gap: 15px; flex-wrap: wrap;">
                <button class="btn btn-success btn-large" onclick="downloadCsv()" style="flex: 1; min-width: 200px; font-size: 1.2em; padding: 15px 25px;">
                    CSV エクスポート
                </button>
                <button class="btn btn-primary btn-large" onclick="downloadSalesSummaryLite()" style="flex: 1; min-width: 200px; font-size: 1.2em; padding: 15px 25px;">
                    売上サマリ(Lite)出力
                </button>
                <button class="btn btn-warning btn-large" onclick="restoreLatest()" style="flex: 1; min-width: 200px; font-size: 1.2em; padding: 15px 25px;">
                    復旧ボタン
                </button>
                <button class="btn btn-info btn-large" onclick="downloadSnapshotJson()" style="flex: 1; min-width: 200px; font-size: 1.2em; padding: 15px 25px;">
                    スナップショット確認
                </button>
            </div>
            <div class="memory-monitor" style="margin-top: 30px; padding: 20px; background: #f8f9fa; border-radius: 8px;">
                <h3 style="margin-bottom: 10px;">メモリ使用状況</h3>
                <div style="display: flex; flex-direction: column; gap: 8px;">
                    <div style="font-size: 1.6em; font-weight: bold; color: #007bff;">
                        空きメモリ容量: <span id="memory-free-heap">-- KB</span>
                    </div>
                    <div style="color: #666;">最小空き: <span id="memory-min-heap">-- KB</span></div>
                    <div style="color: #666;">最大連続割当: <span id="memory-max-alloc">-- KB</span></div>
                    <div style="color: #999; font-size: 0.9em;">最終更新: <span id="memory-last-updated">--:--:--</span></div>
                    <div id="memory-status-message" style="color: #d32f2f; font-size: 0.9em;"></div>
                </div>
            </div>
            
            <div id="api-result" style="margin-top: 20px;"></div>
        </div>
    `;
}

function setupPageEvents() {
    updateCartDisplay();
    const completedWidget = document.getElementById('completed-orders-widget');
    if (completedWidget && completedWidget.style.display !== 'none') {
        loadCompletedOrders();
    }
    if (state.page === 'call') {
        loadCallList();
    }
    if (state.page === 'pickup') {
        document.addEventListener('click', handlePickupButtonClick);
    }
    if (state.page === 'export') {
        startMemoryMonitor();
    } else {
        stopMemoryMonitor();
    }
    if (state.settingsTab === 'sales') {
        ensureSalesSummary();
    }
}

async function updateMemoryStatus() {
    const freeElem = document.getElementById('memory-free-heap');
    if (!freeElem) {
        return;
    }

    const minElem = document.getElementById('memory-min-heap');
    const maxElem = document.getElementById('memory-max-alloc');
    const updatedElem = document.getElementById('memory-last-updated');
    const messageElem = document.getElementById('memory-status-message');

    try {
        const response = await fetch('/api/system/memory');
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const data = await response.json();
        const freeHeap = data.freeHeap ?? data.free_heap ?? 0;
        const minHeap = data.minFreeHeap ?? data.min_free_heap ?? null;
        const maxAlloc = data.maxAllocHeap ?? data.max_alloc_heap ?? null;

        const formatKb = value => `${Math.round(value / 1024)} KB`;

        freeElem.textContent = formatKb(freeHeap);
        if (minElem && minHeap != null) {
            minElem.textContent = formatKb(minHeap);
        }
        if (maxElem && maxAlloc != null) {
            maxElem.textContent = formatKb(maxAlloc);
        }
        if (updatedElem) {
            const now = new Date();
            updatedElem.textContent = now.toLocaleTimeString('ja-JP');
            state.memory = {
                freeHeap,
                minHeap,
                maxAlloc,
                updatedAt: now
            };
        }
        if (messageElem) {
            messageElem.textContent = '';
        }
    } catch (error) {
        console.error('メモリ情報取得エラー:', error);
        freeElem.textContent = '取得エラー';
        if (minElem) minElem.textContent = '-- KB';
        if (maxElem) maxElem.textContent = '-- KB';
        if (messageElem) {
            messageElem.textContent = `メモリ情報の取得に失敗しました: ${error.message}`;
        }
    }
}

function startMemoryMonitor() {
    updateMemoryStatus();
    if (memoryMonitorTimer) {
        clearInterval(memoryMonitorTimer);
    }
    memoryMonitorTimer = setInterval(updateMemoryStatus, 20000);
}

function stopMemoryMonitor() {
    if (memoryMonitorTimer) {
        clearInterval(memoryMonitorTimer);
        memoryMonitorTimer = null;
    }
}

function handlePickupButtonClick(event) {
    const cookedBtn = event.target.closest('.btn-success');
    const pickedBtn = event.target.closest('.btn-info');
    
    if (cookedBtn || pickedBtn) {
        event.stopPropagation();
        
        const orderCard = event.target.closest('.pickup-card');
        if (!orderCard) return;
        const orderNo = orderCard.getAttribute('data-order-no');
        if (!orderNo) {
            console.error('注文番号が見つかりません');
            return;
        }
        if (cookedBtn) {
            updateOrderStatus(orderNo, 'DONE');
        } else if (pickedBtn) {
            updateOrderStatus(orderNo, 'READY');
        }
    }
}

async function loadCallList() {
    try {
        console.log('呼び出しリスト取得開始...');
        const response = await fetch('/api/call-list');
        const data = await response.json();
        state.callList = data.callList || [];
        console.log('呼び出しリスト取得完了:', state.callList.length, '件', state.callList);
        
        if (state.page === 'call') {
            updateCallScreen();
        }
    } catch (error) {
        console.error('呼び出しリスト取得エラー:', error);
    }
}

function toggleFullscreen() {
    if (!document.fullscreenElement) {
        document.documentElement.requestFullscreen().catch(err => {
            console.error('全画面表示エラー:', err);
        });
    } else {
        if (document.exitFullscreen) {
            document.exitFullscreen();
        }
    }
}

function showOrderSuccessModal(orderNo) {
    const existingModal = document.getElementById('order-success-modal');
    if (existingModal) {
        existingModal.remove();
    }
    const modal = document.createElement('div');
    modal.id = 'order-success-modal';
    modal.className = 'modal-backdrop';
    modal.innerHTML = `
        <div class="modal-content" style="text-align: center; padding: 30px;">
            <div style="font-size: 4em; color: #28a745; margin-bottom: 20px;">✅</div>
            <h2 style="color: #28a745; margin-bottom: 15px;">注文確定</h2>
            <p style="font-size: 1.5em; font-weight: bold; margin: 20px 0;">
                注文番号: <span style="color: #007bff; font-size: 2em;">#${orderNo}</span>
            </p>
            <p style="color: #666; margin: 15px 0;">
                注文が正常に登録されました。<br>
                キッチン画面で確認できます。
            </p>
            <button class="btn btn-primary btn-large" onclick="closeOrderSuccessModal()" style="margin-top: 20px; font-size: 1.2em; padding: 12px 30px;">
                OK
            </button>
        </div>
    `;
    
    document.body.appendChild(modal);
    setTimeout(() => {
        closeOrderSuccessModal();
    }, 3000);
}
function closeOrderSuccessModal() {
    const modal = document.getElementById('order-success-modal');
    if (modal) {
        modal.remove();
    }
}
function addMainSingle(sku, priceMode) {
    try {
        if (state.data.printer.paperOut) {
            alert('プリンターの用紙を確認してください');
            return;
        }
        
        const button = event.target;
        if (button.disabled) return;
        button.disabled = true;
        
        state.cart.push({
            type: 'MAIN_SINGLE',
            mainSku: sku,
            priceMode: priceMode,
            qty: 1
        });
        
        button.style.backgroundColor = '#28a745';
        const originalText = button.textContent;
        button.textContent = '追加完了!';
        
        updateCartDisplay();
        
        setTimeout(() => {
            button.disabled = false;
            button.style.backgroundColor = '';
            button.textContent = originalText;
        }, 1000);
        
    } catch (error) {
        console.error('カート追加エラー:', error);
        alert('注文の追加に失敗しました。');
        event.target.disabled = false;
    }
}

function showSideSelectModal(mainSku, priceMode) {
    const sideItems = state.data.menu.filter(item => item.category === 'SIDE' && item.active);
    
    if (sideItems.length === 0) {
        alert('利用可能なサイド商品がありません');
        return;
    }
    
    const mainItem = state.data.menu.find(item => item.sku === mainSku);
    if (!mainItem) return;
    
    const mainPrice = priceMode === 'presale' ? 
        mainItem.price_normal + mainItem.presale_discount_amount : 
        mainItem.price_normal;
    
    const modalHtml = `
        <div class="modal-backdrop" onclick="closeSideSelectModal()">
            <div class="modal-content" onclick="event.stopPropagation()" style="max-width: 600px;">
                <div class="card">
                    <div class="modal-header">
                        <h3>サイド商品を選択</h3>
                        <button class="btn-close" onclick="closeSideSelectModal()">&times;</button>
                    </div>
                    <div class="modal-body">
                        <p><strong>メイン:</strong> ${mainItem.name} (${priceMode === 'presale' ? '前売' : '通常'}: ${mainPrice}円)</p>
                        <p style="color: #666; margin-bottom: 15px;">サイド商品を1つ選択してください:</p>
                        <div class="side-select-grid" style="display: grid; gap: 10px;">
                            ${sideItems.map(side => `
                                <button class="btn btn-secondary" 
                                        onclick="addSetToCart('${mainSku}', '${priceMode}', '${side.sku}')"
                                        style="width: 100%; padding: 15px; text-align: left;">
                                    <div style="display: flex; justify-content: space-between; align-items: center;">
                                        <span style="font-size: 1.1em; font-weight: bold;">${side.name}</span>
                                        <span style="color: #28a745; font-weight: bold;">+${side.price_as_side}円</span>
                                    </div>
                                </button>
                            `).join('')}
                        </div>
                    </div>
                </div>
            </div>
        </div>
    `;
    
    const modal = document.createElement('div');
    modal.id = 'side-select-modal';
    modal.innerHTML = modalHtml;
    document.body.appendChild(modal);
}

function closeSideSelectModal() {
    const modal = document.getElementById('side-select-modal');
    if (modal) {
        modal.remove();
    }
}

function addSetToCart(mainSku, priceMode, sideSku) {
    try {
        if (state.data.printer.paperOut) {
            alert('プリンターの用紙を確認してください');
            return;
        }
        
        state.cart.push({
            type: 'SET',
            mainSku: mainSku,
            priceMode: priceMode,
            sideSkus: [sideSku], 
            qty: 1
        });
        
        updateCartDisplay();
        closeSideSelectModal();
        const mainItem = state.data.menu.find(item => item.sku === mainSku);
        const sideItem = state.data.menu.find(item => item.sku === sideSku);
        if (mainItem && sideItem) {
            alert(`✅ セットを追加しました\n${mainItem.name} + ${sideItem.name}`);
        }
        
    } catch (error) {
        console.error('セット追加エラー:', error);
        alert('セットの追加に失敗しました。');
    }
}

function addToCart(type, sku, priceMode = '') {
    try {
        if (state.data.printer.paperOut) {
            alert('プリンターの用紙を確認してください');
            return;
        }
        const button = event.target;
        if (button.disabled) return;
        button.disabled = true;
        
        if (type === 'SET') {
            console.log('メニューデータ確認:', state.data.menu);
            const sideItems = state.data.menu.filter(item => item.category === 'SIDE' && item.active);
            console.log('フィルタ後のサイドアイテム:', sideItems);
            const selectedSides = sideItems.slice(0, 2).map(item => item.sku);
            console.log('選択されたサイドSKU:', selectedSides);
            
            console.log('SETカート追加:', {
                type: 'SET',
                mainSku: sku,
                priceMode: priceMode,
                sideSkus: selectedSides,
                qty: 1
            });
            
            state.cart.push({
                type: 'SET',
                mainSku: sku,
                priceMode: priceMode,
                sideSkus: selectedSides,
                qty: 1
            });
            button.style.backgroundColor = '#28a745';
            button.textContent = '追加完了!';
            
        } else if (type === 'SIDE_SINGLE') {
            state.cart.push({
                type: 'SIDE_SINGLE',
                sideSku: sku,
                qty: 1
            });
            button.style.backgroundColor = '#28a745';
            button.textContent = '追加完了!';
        }
        
        updateCartDisplay();
        setTimeout(() => {
            button.disabled = false;
            button.style.backgroundColor = '';
            button.textContent = button.textContent.includes('通常') ? '通常で追加' : 
                                button.textContent.includes('前売') ? '前売で追加' : '追加';
        }, 1000);
        
    } catch (error) {
        console.error('カート追加エラー:', error);
        alert('注文の追加に失敗しました。再試行してください。');
        const button = event.target;
        button.disabled = false;
    }
}

function clearCart() {
    state.cart = [];
    updateCartDisplay();
}

function updateCartDisplay() {
    const cartDiv = document.getElementById('cart-items');
    if (!cartDiv || !state.data) return;
    console.log('=== カート表示デバッグ ===');
    console.log('カート内容:', state.cart);
    console.log('メニューデータ数:', state.data.menu ? state.data.menu.length : 0);
    
    if (state.cart.length === 0) {
        cartDiv.innerHTML = '<p>カートは空です</p>';
        return;
    }
    
    const chinchoiroEnabled = state.data.settings.chinchiro.enabled;
    const multipliers = state.data.settings.chinchiro.multipliers || [1];
    
    let total = 0;
    const itemsHtml = state.cart.map((cartItem, idx) => {
        let itemTotal = 0;
        let basePrice = 0;
        let description = '';
        let isSet = false;
        
        if (cartItem.type === 'MAIN_SINGLE') {
            const mainItem = state.data.menu.find(item => item.sku === cartItem.mainSku);
            if (mainItem) {
                const mainPrice = cartItem.priceMode === 'presale' ? 
                    mainItem.price_normal + mainItem.presale_discount_amount : 
                    mainItem.price_normal;
                itemTotal = mainPrice;
                description = `${mainItem.name} (${cartItem.priceMode === 'presale' ? '前売' : '通常'})`;
            }
            basePrice = itemTotal;
        } else if (cartItem.type === 'SET') {
            isSet = true;
            const mainItem = state.data.menu.find(item => item.sku === cartItem.mainSku);
            if (mainItem) {
                const mainPrice = cartItem.priceMode === 'presale' ? 
                    mainItem.price_normal + mainItem.presale_discount_amount : 
                    mainItem.price_normal;
                itemTotal += mainPrice;
                description = ` ${mainItem.name} (${cartItem.priceMode === 'presale' ? '前売' : '通常'})`;
                
                cartItem.sideSkus.forEach(sideSku => {
                    const sideItem = state.data.menu.find(item => item.sku === sideSku);
                    if (sideItem) {
                        itemTotal += sideItem.price_as_side;
                        description += ` +  ${sideItem.name}`;
                    }
                });
            }
            basePrice = itemTotal;
        } else if (cartItem.type === 'SIDE_SINGLE') {
            const sideItem = state.data.menu.find(item => item.sku === cartItem.sideSku);
            if (sideItem) {
                itemTotal = sideItem.price_single;
                description = `${sideItem.name} (単品)`;
            }
        }
        const chinchoiroMultiplier =
            typeof cartItem.chinchoiroMultiplier === 'number'
                ? cartItem.chinchoiroMultiplier
                : 1.0;
        const chinchoiroResult = cartItem.chinchoiroResult ?? 'なし';
        
        if (isSet && chinchoiroEnabled) {
            const adjustment = calculateChinchoiroAdjustmentClient(basePrice, chinchoiroMultiplier);
            itemTotal = basePrice + adjustment;
            const chinchoiroOptions = multipliers.map(m => {
                const label = getChinchoiroLabel(m);
                const selected = Math.abs(m - chinchoiroMultiplier) < 0.01 ? 'selected' : '';
                return `<option value="${m}" ${selected}>${label}</option>`;
            }).join('');
            
            const chinchoiroSelect = `
                <div style="margin-top: 10px; padding: 10px; background: #fff3cd; border-radius: 5px;">
                    <label style="display: block; margin-bottom: 5px; font-weight: bold;">🎲 ちんちろ結果:</label>
                    <select class="form-control" onchange="applyChinchoiro(${idx}, parseFloat(this.value))" style="padding: 5px;">
                        ${chinchoiroOptions}
                    </select>
                    ${adjustment !== 0 ? `<small style="display: block; margin-top: 5px; color: ${adjustment > 0 ? '#d9534f' : '#5cb85c'};">
                        調整額: ${adjustment > 0 ? '+' : ''}${adjustment}円
                    </small>` : ''}
                </div>
            `;
            
            const lineTotal = itemTotal * cartItem.qty;
            total += lineTotal;
            
            return `
                <div class="cart-item-card" style="border: 2px solid #ffc107; padding: 12px; margin: 8px 0; border-radius: 8px; background: #fffef5;">
                    <p style="margin: 0 0 8px 0; font-weight: bold;">${description}</p>
                    <p style="margin: 0 0 8px 0;">基本価格: ${basePrice}円 × ${cartItem.qty}個</p>
                    ${chinchoiroSelect}
                    <p style="margin: 8px 0 0 0; font-size: 1.1em; font-weight: bold; color: #333;">
                        小計: ${lineTotal}円
                    </p>
                    <button class="btn btn-secondary btn-sm" onclick="removeFromCart(${idx})" style="margin-top: 8px;">削除</button>
                </div>
            `;
        } else {
            const lineTotal = itemTotal * cartItem.qty;
            total += lineTotal;
            
            return `
                <div style="border: 1px solid #ddd; padding: 10px; margin: 5px 0; border-radius: 5px;">
                    <p style="margin: 0 0 8px 0;">${description}</p>
                    <p style="margin: 0;">数量: ${cartItem.qty} × ${itemTotal}円 = ${lineTotal}円</p>
                    <button class="btn btn-secondary btn-sm" onclick="removeFromCart(${idx})" style="margin-top: 5px;">削除</button>
                </div>
            `;
        }
    }).join('');
    
    cartDiv.innerHTML = itemsHtml + `<p style="margin-top: 15px; font-size: 1.3em;"><strong>合計: ${total}円</strong></p>`;
    updateConfirmOrderButton();
}

function getChinchoiroLabel(multiplier) {
    if (multiplier === 0) return 'ピンゾロ（無料）';
    if (multiplier === 0.5) return '半額';
    if (multiplier === 1.0) return 'なし（通常）';
    if (multiplier === 2.0) return '2倍';
    if (multiplier === 3.0) return '3倍';
    return `${multiplier}倍`;
}

function calculateChinchoiroAdjustmentClient(basePrice, multiplier) {
    const rounding = state.data.settings.chinchiro.rounding || 'round';
    const rawAdjustment = basePrice * (multiplier - 1.0);
    
    if (rounding === 'floor') {
        return Math.floor(rawAdjustment);
    } else if (rounding === 'ceil') {
        return Math.ceil(rawAdjustment);
    } else {
        return Math.round(rawAdjustment);
    }
}

function applyChinchoiro(cartIndex, multiplier) {
    if (cartIndex < 0 || cartIndex >= state.cart.length) return;
    
    const cartItem = state.cart[cartIndex];
    cartItem.chinchoiroMultiplier = multiplier;
    cartItem.chinchoiroResult = getChinchoiroLabel(multiplier);
    
    updateCartDisplay();
}

function removeFromCart(index) {
    state.cart.splice(index, 1);
    updateCartDisplay();
}

function updateConfirmOrderButton() {
    const btn = document.querySelector('#confirm-order-btn, [data-action="confirm-order"]');
    if (!btn) return;
    const shouldDisable = (state.cart.length === 0) || (state?.data?.printer?.paperOut);
    btn.disabled = shouldDisable;
}

function safeNum(v) { 
    const n = Number(v); 
    return Number.isFinite(n) ? n : 0; 
}

async function handleConfirmOrder(event) {
    event.preventDefault();
    event.stopPropagation();
    
    const button = event.target;

    if (button.dataset.loading === "1" || button.disabled) {
        console.log('注文送信中またはボタン無効 - スキップ');
        return;
    }
    
    console.log('handleConfirmOrder: 注文確定処理開始');
    button.dataset.loading = "1";
    
    try {
        await submitOrder();
    } catch (error) {
        console.error('注文確定エラー:', error);
    } finally {
        delete button.dataset.loading;
    }
}

async function submitOrder() {
    console.log('=== submitOrder 呼び出し開始 ===');
    console.log('カート内容:', state.cart);
    console.log('カートサイズ:', state.cart.length);
    
    if (state.cart.length === 0) {
        alert('カートが空です');
        return;
    }

    const submitBtn = document.querySelector('[data-action="confirm-order"]');
    console.log('ボタン状態:', submitBtn ? `disabled=${submitBtn.disabled}` : 'ボタンが見つからない');
    if (submitBtn) {
        submitBtn.disabled = true;
        submitBtn.textContent = '⏳ 注文処理中...';
        console.log('ボタンをdisabledに設定');
    }

    const safeCart = state.cart.map(item => {
        const safeItem = { ...item };
        if ('qty' in safeItem) safeItem.qty = Math.max(1, safeNum(safeItem.qty));
        if ('unitPriceApplied' in safeItem) safeItem.unitPriceApplied = safeNum(safeItem.unitPriceApplied);
        if ('unitPrice' in safeItem) safeItem.unitPrice = safeNum(safeItem.unitPrice);
        if ('discountValue' in safeItem) safeItem.discountValue = safeNum(safeItem.discountValue);
        return safeItem;
    });
    
    console.log('NaN耐性処理後のカート:', safeCart);

    const maxRetries = 3;
    let retryCount = 0;
    
    while (retryCount < maxRetries) {
        try {
            const response = await fetch('/api/orders', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ lines: safeCart }),
                timeout: 10000 
            });
            
        if (response.ok) {
            const result = await response.json();

            console.log('=== 注文送信デバッグ ===');
            console.log('送信データ:', { lines: state.cart });
            console.log('サーバー応答:', result);

            clearCart();
            await loadStateData(); 
            updateConfirmOrderButton();

            showOrderSuccessModal(result.orderNo);

            if (submitBtn) {
                submitBtn.disabled = false;
                submitBtn.style.backgroundColor = '';
                submitBtn.textContent = '📝 注文確定';
            }
            
            return;
            
        } else {
                const errorData = await response.text();
                let errorMsg;
                try {
                    const errorJson = JSON.parse(errorData);
                    errorMsg = errorJson.error || '不明なエラー';
                } catch {
                    errorMsg = `HTTP ${response.status}: ${errorData}`;
                }
                throw new Error(errorMsg);
            }
            
        } catch (error) {
            retryCount++;
            console.error(`注文送信失敗 (試行${retryCount}/${maxRetries}):`, error);
            
            if (retryCount < maxRetries) {
                await new Promise(resolve => setTimeout(resolve, 1000));
                submitBtn.textContent = `再試行中... (${retryCount + 1}/${maxRetries})`;
            } else {
                alert(`注文の送信に失敗しました: ${error.message}\n\nカートの内容は保持されています。再度お試しください。`);
                if (submitBtn) {
                    submitBtn.disabled = false;
                    submitBtn.style.backgroundColor = '#dc3545';
                    submitBtn.textContent = '📝 注文確定（再試行）';
                    setTimeout(() => {
                        submitBtn.style.backgroundColor = '';
                        submitBtn.textContent = '📝 注文確定';
                    }, 5000);
                }
            }
        }
    }
}

async function cancelOrder(orderNo) {
    console.log('[cancelOrder] 開始: 注文番号=', orderNo, 'タイプ=', typeof orderNo);
    
    const resolved = getOrderFromState(orderNo);
    if (!resolved) {
        console.error('[cancelOrder] 注文が見つかりません:', orderNo);
        alert(`❌ 注文 #${orderNo} が見つかりません`);
        return;
    }

    const { order, source } = resolved;
    console.log('[cancelOrder] 注文データ検索結果:', { ...order, __source: source });
    if (order.status === 'CANCELLED') {
        alert(`❌ 注文 #${orderNo} は既にキャンセル済みです`);
        return;
    }
    
    // キャンセル理由を入力
    const reason = prompt('キャンセル理由を入力してください（任意）:') || '';
    
    try {
        const payload = { orderNo, reason };
        console.log('[cancelOrder] 送信データ(JSON):', payload);
        
        const response = await fetch('/api/orders/cancel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        
        console.log('[cancelOrder] レスポンス:', response.status, response.statusText);
        
        if (response.ok) {
            const data = await response.json();
            console.log('[cancelOrder] キャンセル成功:', data);
            alert(`✅ 注文 #${orderNo} をキャンセルしました`);
            // 画面を更新
            await loadStateData();
            ensureArchivedOrders(true);
        } else {
            // エラーレスポンスの処理
            let errorMessage = `HTTP ${response.status}`;
            try {
                const errorData = await response.json();
                errorMessage = errorData.error || errorMessage;
            } catch {
                const errorText = await response.text();
                errorMessage = errorText || errorMessage;
            }
            console.error('[cancelOrder] キャンセル失敗:', errorMessage);
            alert(`❌ キャンセルに失敗しました: ${errorMessage}`);
        }
    } catch (error) {
        console.error('[cancelOrder] 通信エラー:', error);
        alert(`❌ 通信エラー: ${error.message}\n\nネットワーク接続を確認してください。`);
    }
}

async function completeOrder(orderNo) {
    try {
        const response = await fetch('/api/orders/update', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ orderNo: orderNo, status: 'DONE' })
        });
        
        if (response.ok) {
            alert(`注文 # ${orderNo} を完了しました`);
            loadStateData();
        } else {
            alert('完了処理に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

function switchSettingsTab(tab) {
    state.settingsTab = tab;
    if (tab === 'sales') {
        ensureSalesSummary();
    }
    render();
}

async function saveMainProducts() {
    const items = [];
    const mainItems = state.data.menu.filter(item => item.category === 'MAIN');
    mainItems.forEach((item, idx) => {
        items.push({
            id: document.getElementById(`main-id-${idx}`).value,
            name: document.getElementById(`main-name-${idx}`).value,
            nameRomaji: document.getElementById(`main-name-romaji-${idx}`).value,
            price_normal: parseInt(document.getElementById(`main-normal-${idx}`).value) || 0,
            presale_discount_amount: parseInt(document.getElementById(`main-discount-${idx}`).value) || 0,
            active: document.getElementById(`main-active-${idx}`).checked
        });
    });
    const newName = document.getElementById('main-name-new').value;
    if (newName) {
        items.push({
            name: newName,
            nameRomaji: document.getElementById('main-name-romaji-new').value,
            price_normal: parseInt(document.getElementById('main-normal-new').value) || 0,
            presale_discount_amount: parseInt(document.getElementById('main-discount-new').value) || 0,
            active: document.getElementById('main-active-new').checked
        });
    }
    
    try {
        const response = await fetch('/api/products/main', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items })
        });
        
        if (response.ok) {
            alert('メイン商品を保存しました');
            await loadMenu({ force: true });
            await loadStateData({ forceFull: true });
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

async function saveSystemSettings() {
    const settings = {
        presaleEnabled: document.getElementById('presale-enabled').checked,
        store: {
            name: document.getElementById('store-name').value,
            nameRomaji: document.getElementById('store-name-romaji').value,
            registerId: document.getElementById('register-id').value
        },
        numbering: {
            min: parseInt(document.getElementById('numbering-min').value) || 1,
            max: parseInt(document.getElementById('numbering-max').value) || 9999
        }
    };
    
    try {
        const response = await fetch('/api/settings/system', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(settings)
        });
        
        if (response.ok) {
            alert('システム設定を保存しました');
            loadStateData(); 
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

async function saveSideProducts() {
    const items = [];
    const sideItems = state.data.menu.filter(item => item.category === 'SIDE');
    sideItems.forEach((item, idx) => {
        items.push({
            id: document.getElementById(`side-id-${idx}`).value,
            name: document.getElementById(`side-name-${idx}`).value,
            nameRomaji: document.getElementById(`side-name-romaji-${idx}`).value,
            price_single: parseInt(document.getElementById(`side-single-${idx}`).value) || 0,
            price_as_side: parseInt(document.getElementById(`side-set-${idx}`).value) || 0,
            active: document.getElementById(`side-active-${idx}`).checked
        });
    });
    const newName = document.getElementById('side-name-new').value;
    if (newName) {
        items.push({
            name: newName,
            nameRomaji: document.getElementById('side-name-romaji-new').value,
            price_single: parseInt(document.getElementById('side-single-new').value) || 0,
            price_as_side: parseInt(document.getElementById('side-set-new').value) || 0,
            active: document.getElementById('side-active-new').checked
        });
    }
    
    try {
        const response = await fetch('/api/products/side', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items })
        });
        
        if (response.ok) {
            alert('サイド商品を保存しました');
            await loadMenu({ force: true });
            await loadStateData({ forceFull: true });
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

async function saveChinchoiroSettings() {
    const enabled = document.getElementById('chinchiro-enabled').checked;
    const multipliers = document.getElementById('chinchiro-multipliers').value
        .split(',').map(s => parseFloat(s.trim())).filter(n => !isNaN(n));
    const rounding = document.getElementById('chinchiro-rounding').value;
    
    try {
        const response = await fetch('/api/settings/chinchiro', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ enabled, multipliers, rounding })
        });
        
        if (response.ok) {
            alert('ちんちろ設定を保存しました');
            loadStateData();
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

async function saveQrPrintSettings() {
    const enabled = document.getElementById('qrprint-enabled').checked;
    const content = document.getElementById('qrprint-content').value.trim();
    
    try {
        const response = await fetch('/api/settings/qrprint', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ enabled, content })
        });
        
        if (response.ok) {
            alert('QRプリント設定を保存しました');
            loadStateData();
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

function updateMainItem(sku, field, value) {
    if (!state.data) return;
    const item = state.data.menu.find(m => m.sku === sku && m.category === 'MAIN');
    if (item) {
        item[field] = value;
        debouncedSaveMenuItem(item);
    }
}

function updateSideItem(sku, field, value) {
    if (!state.data) return;
    const item = state.data.menu.find(m => m.sku === sku && m.category === 'SIDE');
    if (item) {
        item[field] = value;
        debouncedSaveMenuItem(item);
    }
}

function toggleMainItemActive(sku, active) {
    if (!state.data) return;
    const item = state.data.menu.find(m => m.sku === sku && m.category === 'MAIN');
    if (item) {
        item.active = active;
        saveMenuItemImmediate(item);
    }
}

function toggleSideItemActive(sku, active) {
    if (!state.data) return;
    const item = state.data.menu.find(m => m.sku === sku && m.category === 'SIDE');
    if (item) {
        item.active = active;
        saveMenuItemImmediate(item);
    }
}

function addNewMainItem() {
    const name = prompt('新しいメイン商品名を入力してください:');
    if (!name) return;
    
    const nameRomaji = prompt('商品名（ローマ字）を入力してください:', name);
    const priceNormal = parseInt(prompt('通常価格を入力してください:', '500') || '500');
    const presaleDiscount = parseInt(prompt('前売割引額を入力してください（マイナス値）:', '-100') || '-100');
    
    const newItem = {
        name: name,
        nameRomaji: nameRomaji || name,
        category: 'MAIN',
        active: true,
        price_normal: priceNormal,
        presale_discount_amount: presaleDiscount,
        price_presale: 0,
        price_single: 0,
        price_as_side: 0
    };
    
    saveNewMenuItem(newItem);
}

function addNewSideItem() {
    const name = prompt('新しいサイド商品名を入力してください:');
    if (!name) return;
    
    const nameRomaji = prompt('商品名（ローマ字）を入力してください:', name);
    const priceSingle = parseInt(prompt('単品価格を入力してください:', '200') || '200');
    const priceAsside = parseInt(prompt('セット時価格を入力してください:', '100') || '100');
    
    const newItem = {
        name: name,
        nameRomaji: nameRomaji || name,
        category: 'SIDE',
        active: true,
        price_normal: 0,
        presale_discount_amount: 0,
        price_presale: 0,
        price_single: priceSingle,
        price_as_side: priceAsside
    };
    
    saveNewMenuItem(newItem);
}

async function saveNewMenuItem(item) {
    try {
        const endpoint = item.category === 'MAIN' ? '/api/products/main' : '/api/products/side';
        const response = await fetch(endpoint, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items: [item] })
        });
        
        if (response.ok) {
            alert('商品を追加しました');
            await loadStateData();
            render();
        } else {
            alert('商品の追加に失敗しました');
        }
    } catch (error) {
        console.error('商品追加エラー:', error);
        alert('通信エラーが発生しました');
    }
}

let saveMenuTimer = null;

function debouncedSaveMenu() {
    if (saveMenuTimer) clearTimeout(saveMenuTimer);
    saveMenuTimer = setTimeout(() => {
        saveMenuImmediate();
    }, 1000); 
}

async function saveMenuItemImmediate(item) {
    if (!item || !item.sku) {
        console.error('SKUが見つかりません:', item);
        return;
    }
    
    try {
        const endpoint = item.category === 'MAIN' ? '/api/products/main' : '/api/products/side';
        const payload = {
            items: [{
                id: item.sku, 
                name: item.name,
                nameRomaji: item.nameRomaji || item.name,
                active: item.active,
                ...(item.category === 'MAIN' ? {
                    price_normal: item.price_normal,
                    presale_discount_amount: item.presale_discount_amount
                } : {
                    price_single: item.price_single,
                    price_as_side: item.price_as_side
                })
            }]
        };
        
        const response = await fetch(endpoint, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        
        if (response.ok) {
            console.log(`✅ 商品を更新しました: ${item.name} (${item.sku})`);
            await loadMenu({ force: true });
        } else {
            console.error('❌ 商品の更新に失敗しました:', await response.text());
        }
    } catch (error) {
        console.error('❌ 商品更新エラー:', error);
    }
}

let saveMenuItemTimer = null;
function debouncedSaveMenuItem(item) {
    if (saveMenuItemTimer) {
        clearTimeout(saveMenuItemTimer);
    }
    saveMenuItemTimer = setTimeout(() => {
        saveMenuItemImmediate(item);
    }, 1000); 
}


async function saveMenuImmediate() {
    console.warn('⚠️ saveMenuImmediate()は非推奨です。個別更新を使用してください。');
}

async function downloadCsv() {
    try {
        const response = await fetch('/api/export/csv');
        if (!response.ok) throw new Error('CSVエクスポート失敗');
        const blob = await response.blob();
        const url = window.URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'sales.csv';
        document.body.appendChild(a);
        a.click();
        setTimeout(() => {
            window.URL.revokeObjectURL(url);
            document.body.removeChild(a);
        }, 1000);
        setTimeout(() => {
            showSessionEndDialog();
        }, 2000);
    } catch (error) {
        console.error('CSVエクスポートエラー:', error);
        alert('CSVエクスポートに失敗しました');
    }
}

function downloadSalesSummaryLite() {
    window.open('/api/export/sales-summary-lite', '_blank');
}

function downloadSnapshotJson() {
    window.open('/api/export/snapshot', '_blank');
}

function showSessionEndDialog() {
    const modal = document.createElement('div');
    modal.className = 'modal-overlay';
    modal.innerHTML = `
        <div class="modal-content session-dialog">
            <h2>営業データのエクスポートが完了しました</h2>
            <p>今後の営業をどうしますか？</p>
            <p class="session-note">📶 「売上確認画面を開く」を押すとKDSのソフトAPが60秒停止します。アップロード後は自動で再開します。</p>
            <div class="session-options">
                <button class="btn btn-success btn-large" onclick="continueSession()">
                    🔄 営業を続ける
                    <small>現在のデータをそのまま継続</small>
                </button>
                <button class="btn btn-primary btn-large" onclick="openSalesSummaryUploader()">
                    📤 売上確認画面を開く
                    <small>タップするとAPが60秒停止（外部サイト）</small>
                </button>
                <button class="btn btn-warning btn-large" onclick="confirmEndSession()">
                    🏁 営業セッション終了
                    <small>データを初期化して新しいセッションを開始</small>
                </button>
            </div>
        </div>
    `;
    
    document.body.appendChild(modal);
    
    modal.addEventListener('click', (e) => {
        if (e.target === modal) {
            continueSession();
        }
    });
}

function continueSession() {
    closeSessionDialog();
    alert('営業を継続します。現在のデータが保持されます。');
}

function confirmEndSession() {
    const confirmed = confirm(
        '⚠️ 営業セッション終了の確認\n\n' +
        '本当に営業セッションを終了しますか？\n' +
        '• 全ての注文データが削除されます\n' +
        '• メニュー設定は保持されます\n' +
        '• 新しいセッション登録画面に移動します\n\n' +
        'この操作は取り消せません。'
    );
    
    if (confirmed) {
        endSession();
    }
}

async function endSession() {
    try {
        const response = await fetch('/api/session/end', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        });
        
        if (response.ok) {
            const modal = document.querySelector('.modal-overlay');
            if (modal) {
                document.body.removeChild(modal);
            }
            alert('🎉 営業セッションが終了しました。\n新しいセッションを開始してください。');
            await loadStateData();
            state.page = 'order'; 
            render();
            
        } else {
            const error = await response.text();
            alert(`セッション終了に失敗しました: ${error}`);
        }
        
    } catch (error) {
        console.error('セッション終了エラー:', error);
        alert(`通信エラー: ${error.message}`);
    }
}

async function restoreLatest() {
    if (!confirm('最新のスナップショット + WAL ログから復元しますか？\n\n※電源断前の状態に戻ります')) return;
    
    const resultDiv = document.getElementById('api-result');
    if (resultDiv) {
        resultDiv.innerHTML = '<div class="card"><p>⏳ 復元処理中...</p></div>';
    }
    
    try {
        const response = await fetch('/api/recover', { method: 'POST' });
        const result = await response.json();
        
        if (result.ok) {
            console.log('復元成功:', result);
            if (resultDiv) {
                resultDiv.innerHTML = `
                    <div class="card" style="border-left-color: #28a745;">
                        <h3>✅ 復旧成功</h3>
                        <p><strong>適用時刻:</strong> ${result.lastTs || '-'}</p>
                        <p style="color: #666; margin-top: 10px;">スナップショット + WAL ログから状態を復元しました。<br>画面を更新して最新状態を確認してください。</p>
                    </div>
                `;
            }
            // 最新状態を取得してUI同期
            await loadStateData();
            alert(`✅ 復元完了\n\n適用時刻: ${result.lastTs}`);
        } else {
            console.error('復元失敗:', result.error);
            if (resultDiv) {
                resultDiv.innerHTML = `
                    <div class="card" style="border-left-color: #dc3545;">
                        <h3>❌ 復旧失敗</h3>
                        <p><strong>エラー:</strong> ${result.error || 'unknown error'}</p>
                        <p style="color: #666; margin-top: 10px;">復元処理中にエラーが発生しました。</p>
                    </div>
                `;
            }
            alert(`❌ 復元失敗: ${result.error}`);
        }
    } catch (error) {
        console.error('通信エラー:', error);
        if (resultDiv) {
            resultDiv.innerHTML = `
                <div class="card" style="border-left-color: #dc3545;">
                    <h3>❌ 復旧失敗</h3>
                    <p><strong>エラー:</strong> ${error.message}</p>
                    <p style="color: #666; margin-top: 10px;">通信エラーが発生しました。ネットワーク接続を確認してください。</p>
                </div>
            `;
        }
        alert(`通信エラー: ${error.message}`);
    }
}

function showOrderDetail(orderNo) {
    if (!state.data) return;

    const resolved = getOrderFromState(orderNo);
    if (!resolved) return;

    const { order, source } = resolved;
    
    const totalAmount = order.items.reduce((sum, item) => {
        const unitPrice = item.unitPriceApplied || item.unitPrice || 0;
        const qty = item.qty || 1;
        const discount = item.discountValue || 0;
        return sum + (unitPrice * qty - discount);
    }, 0);
    
    const itemsList = order.items.map(item => {
        const unitPrice = item.unitPriceApplied || item.unitPrice || 0;
        const qty = item.qty || 1;
        const discount = item.discountValue || 0;
        const lineTotal = unitPrice * qty - discount;
        return `
            <div class="order-item-row">
                <span>${item.name} x${qty}</span>
                <span>¥${lineTotal}</span>
            </div>
        `;
    }).join('');
    
    const statusActions = getStatusActions(order);
    
    const modalHtml = `
        <div class="modal-backdrop" onclick="closeModal()">
            <div class="modal-content" onclick="event.stopPropagation()">
                <div class="card">
                    <div class="modal-header">
                        <h3>#${order.orderNo}</h3>
                        <button class="btn-close" onclick="closeModal()">&times;</button>
                    </div>
                    <div class="modal-body">
                        <p><strong>状態:</strong> ${getStatusLabel(order.status)}</p>
                        <p><strong>注文時刻:</strong> ${order.ts && order.ts > 946684800 ? new Date(order.ts * 1000).toLocaleString() : '時刻不明'}</p>
                        <p><strong>ソース:</strong> ${source === 'archived' ? 'アーカイブ' : 'リアルタイム'}</p>
                        <div class="order-items">
                            <h4>注文内容</h4>
                            ${itemsList}
                            <div class="order-total">
                                <strong>合計: ¥${totalAmount}</strong>
                            </div>
                        </div>
                        ${statusActions}
                    </div>
                </div>
            </div>
        </div>
    `;
    
    const modal = document.createElement('div');
    modal.innerHTML = modalHtml;
    document.body.appendChild(modal);
}

function closeModal() {
    const modal = document.querySelector('.modal-backdrop');
    if (modal) {
        modal.remove();
    }
}

function getStatusActions(order) {
    const actions = [];
    if (order.status === 'COOKING' && !order.cooked) {
        actions.push(`
            <button class="btn btn-warning" onclick="updateOrderStatus('${order.orderNo}', 'DONE')" 
                    style="width: 100%; margin-top: 5px; font-size: 1.6em; padding: 20px 30px;">
                📌 調理完了
            </button>
        `);
    }

    if (order.status === 'DONE' && !order.picked_up) {
        actions.push(`
            <button class="btn btn-primary" onclick="updateOrderStatus('${order.orderNo}', 'READY')" 
                    style="width: 100%; margin-top: 5px; font-size: 1.6em; padding: 20px 30px;">
                📌 品出し完了
            </button>
        `);
    }
    
    return actions.length > 0 ? `
        <div class="modal-actions">
            <div style="background: #f8f9fa; padding: 10px; border-radius: 5px; margin-bottom: 15px; text-align: center;">
                <strong style="color: #007acc;">👇 状態を変更する</strong>
            </div>
            ${actions.join('')}
        </div>
    ` : '';
}

function getStatusLabel(status) {
    const labels = {
        'COOKING': '調理中',
        'DONE': '調理完了',
        'READY': '品出し完了',
        'DELIVERED': '提供済み',
        'CANCELLED': 'キャンセル'
    };
    return labels[status] || status;
}

async function updateOrderStatus(orderNo, newStatus) {
    console.log(`注文状態更新: ${orderNo} → ${newStatus}`);
    // 注文処理中なら完了まで待機
    if (window.activeOrderPromise) {
        try {
            await window.activeOrderPromise;
        } catch (e) {
            // submitOrder失敗時は状態変更もスキップ
            console.error('注文処理失敗のため状態変更スキップ:', e);
            return;
        }
    }
    try {
        const response = await fetch(`/api/orders/${orderNo}`, {
            method: 'PATCH',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ status: newStatus })
        });
        if (response.ok) {
            console.log(`✅ 注文 ${orderNo} を ${newStatus} に更新`);
            closeModal();
            await loadStateData();
            await loadCallList();
        } else {
            const errorText = await response.text();
            console.error(`❌ API失敗: ${errorText}`);
            alert(`状態更新に失敗しました\nStatus: ${response.status}`);
        }
    } catch (error) {
        console.error('状態更新エラー:', error);
        alert(`状態更新に失敗しました\n${error.message}`);
    }
}

function completeOrder(orderNo) {
    console.warn('⚠️ completeOrder は非推奨です。updateOrderStatus を使用してください');
    updateOrderStatus(orderNo, 'DONE');
}

async function resetSystem() {
    if (!confirm('⚠️ 警告: システムを完全初期化します。\n\n• 全ての注文データが削除されます\n• 注文番号カウンタがリセットされます\n• 不揮発性メモリがクリアされます\n\n本当に実行しますか？')) {
        return;
    }
    
    if (!confirm('🔴 最終確認: この操作は取り消せません。\n本当にシステムを初期化しますか？')) {
        return;
    }
    
    try {
        const response = await fetch('/api/system/reset', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        });
        
        if (response.ok) {
            const result = await response.json();
            alert('✅ システム初期化完了\n\n' + result.message);

            await loadStateData();
            
            state.cart = [];
            updateCartDisplay();
            
        } else {
            const errorData = await response.text();
            alert('❌ 初期化に失敗しました: ' + errorData);
        }
    } catch (error) {
        console.error('システム初期化エラー:', error);
        alert('❌ 初期化に失敗しました: ' + error.message);
    }
}

function toggleCompletedOrders() {
    const widget = document.getElementById('completed-orders-widget');
    const button = document.getElementById('toggle-completed-btn');
    
    if (widget.style.display === 'none') {
        widget.style.display = 'block';
        button.textContent = '📋 注文済み一覧非表示';
        loadCompletedOrders();
    } else {
        widget.style.display = 'none';
        button.textContent = '📋 キャンセル・再印刷';
    }
}

function loadCompletedOrders() {
    if (!state.data) return;

    ensureArchivedOrders();

    const merged = new Map();

    if (state.archived && Array.isArray(state.archived.orders)) {
        state.archived.orders.forEach(order => {
            if (!order || !order.orderNo) {
                return;
            }
            if (!merged.has(order.orderNo)) {
                merged.set(order.orderNo, { ...order, __source: 'archived' });
            }
        });
    }

    if (state.data && Array.isArray(state.data.orders)) {
        state.data.orders.forEach(order => {
            if (!order || !order.orderNo) {
                return;
            }
            merged.set(order.orderNo, { ...order, __source: 'active' });
        });
    }

    const completedOrders = Array.from(merged.values())
        .filter(order => ['COOKING', 'DONE', 'READY', 'DELIVERED', 'CANCELLED'].includes(order.status))
        .sort((a, b) => (b.ts || 0) - (a.ts || 0))
        .slice(0, 20);
    
    const listDiv = document.getElementById('completed-orders-list');
    
    if (completedOrders.length === 0) {
        listDiv.innerHTML = '<p>注文履歴がありません</p>';
        return;
    }
    
    listDiv.innerHTML = completedOrders.map(order => {
        const statusLabel = getStatusLabel(order.status);
        const statusColor = getStatusColor(order.status);
        const timeStr = order.ts && order.ts > 946684800 ? 
            new Date(order.ts * 1000).toLocaleString() : '時刻不明';
        const sourceBadge = order.__source === 'archived' ? '<span class="tag" style="background:#6c757d;color:#fff;padding:2px 6px;border-radius:3px;font-size:0.7em;">ARCHIVE</span>' : '';

        const items = Array.isArray(order.items) ? order.items : [];
        const totalAmount = items.reduce((sum, item) => {
            const unitPrice = item.unitPriceApplied || item.unitPrice || 0;
            const qty = item.qty || 1;
            const discount = item.discountValue || 0;
            return sum + (unitPrice * qty - discount);
        }, 0);
        
        const allowReprint = order.status !== 'CANCELLED';
        const allowCancel = ['COOKING', 'DONE', 'READY', 'DELIVERED'].includes(order.status);

        return `
            <div class="completed-order-item" style="border: 1px solid #ddd; margin: 10px 0; padding: 15px; border-radius: 5px;">
                <div class="order-header" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;">
                    <h4 style="margin: 0; color: #333;">注文 #${order.orderNo} ${sourceBadge}</h4>
                    <span class="status-badge" style="background-color: ${statusColor}; color: white; padding: 4px 8px; border-radius: 3px; font-size: 0.8em;">
                        ${statusLabel}
                    </span>
                </div>
                <div class="order-info" style="font-size: 0.9em; color: #666; margin-bottom: 10px;">
                    <div>注文時刻: ${timeStr}</div>
                    <div>合計金額: ${totalAmount}円</div>
                </div>
                <div class="order-items" style="margin-bottom: 15px;">
                    ${items.slice(0, 3).map(item => 
                        `<span style="background: #f8f9fa; padding: 2px 6px; margin: 2px; border-radius: 3px; font-size: 0.8em; display: inline-block;">
                            ${item.name} x${item.qty}
                        </span>`
                    ).join('')}
                    ${items.length > 3 ? '<span style="color: #666;">...</span>' : ''}
                </div>
                <div class="order-actions" style="display: flex; gap: 10px; flex-wrap: wrap;">
                    <button class="btn btn-sm btn-info" onclick="showOrderDetail('${order.orderNo}')" style="font-size: 0.8em;">
                        📄 詳細
                    </button>
                    ${allowReprint ? `
                    <button class="btn btn-sm btn-secondary" onclick="reprintReceipt('${order.orderNo}')" style="font-size: 0.8em;">
                        🖨️ 再印刷
                    </button>` : ''}
                    ${allowCancel ? `
                    <button class="btn btn-sm btn-warning" onclick="cancelOrder('${order.orderNo}')" style="font-size: 0.8em;">
                        ❌ キャンセル
                    </button>` : ''}
                </div>
            </div>
        `;
    }).join('');
}

function getStatusColor(status) {
    const colors = {
        'COOKING': '#ffc107',   
        'DONE': '#28a745',     
        'READY': '#17a2b8',    
        'DELIVERED': '#6c757d',  
        'CANCELLED': '#dc3545'   
    };
    return colors[status] || '#6c757d';
}

function refreshSalesStats() {
    if (state.settingsTab === 'sales') {
        fetchSalesSummary(true);
    }
}

async function reprintReceipt(orderNo) {
    console.log('[reprintReceipt] 開始: 注文番号=', orderNo, 'タイプ=', typeof orderNo);
    
    // 確認ダイアログ
    if (!confirm(`注文 #${orderNo} のレシートを再印刷しますか？`)) {
        console.log('[reprintReceipt] ユーザーがキャンセルしました');
        return;
    }
    
    const resolved = getOrderFromState(orderNo);
    if (!resolved) {
        console.error('[reprintReceipt] 注文が見つかりません:', orderNo);
        alert(`❌ 注文 #${orderNo} が見つかりません`);
        return;
    }

    const { order, source } = resolved;
    console.log('[reprintReceipt] 注文データ検索結果:', { ...order, __source: source });
    if (order.status === 'CANCELLED') {
        alert(`❌ キャンセル済みの注文は再印刷できません\n注文番号: ${orderNo}`);
        return;
    }
    
    try {
        const requestBody = { orderNo: orderNo };
        console.log('[reprintReceipt] 送信データ:', requestBody);
        
        const response = await fetch('/api/orders/reprint', {
            method: 'POST',
            headers: { 
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(requestBody)
        });
        
        console.log('[reprintReceipt] レスポンス:', response.status, response.statusText);
        
        if (response.ok) {
            const result = await response.json();
            console.log('[reprintReceipt] 成功:', result);
            alert(`✅ レシート再印刷を実行しました\n注文番号: ${orderNo}\n\n${result.message || 'プリンタキューに追加しました'}`);
            // 画面を更新
            await loadStateData();
        } else {
            // エラーレスポンスの処理
            let errorMessage = `HTTP ${response.status}`;
            try {
                const errorData = await response.json();
                errorMessage = errorData.error || errorMessage;
            } catch {
                const errorText = await response.text();
                errorMessage = errorText || errorMessage;
            }
            console.error('[reprintReceipt] エラーレスポンス:', errorMessage);
            alert(`❌ 再印刷に失敗しました: ${errorMessage}`);
        }
    } catch (error) {
        console.error('[reprintReceipt] 通信エラー:', error);
        alert(`❌ 通信エラー: ${error.message}\n\nネットワーク接続を確認してください。`);
    }
}

function closeSessionDialog() {
    const modal = document.querySelector('.modal-overlay');
    if (modal) {
        modal.remove();
    }
}

async function openSalesSummaryUploader() {
    const confirmed = confirm(
        '📤 売上確認ツールを開くと、KDSのソフトAPが60秒間停止します。\n' +
        'この間、タブレットは一時的に切断されます。続行しますか？'
    );

    if (!confirmed) {
        return;
    }

    const uploadUrl = 'https://kds-checker.vercel.app/upload';

    try {
        const response = await fetch('/api/network/ap-cycle', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ resumeAfter: 60 })
        });

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const result = await response.json();
        console.log('[openSalesSummaryUploader] AP suspend requested:', result);
    } catch (error) {
        console.error('[openSalesSummaryUploader] Failed to suspend AP:', error);
        if (popup && !popup.closed) {
            popup.close();
        }
        alert('⚠️ ソフトAPの一時停止に失敗しました。ネットワークを確認してから再度お試しください。');
        return;
    }

    closeSessionDialog();

    window.open(uploadUrl, '_blank');

    alert('売上確認ツールを新しいタブで開きました。タブレットは60秒後に自動でKDS Wi-Fiへ再接続します。');
}