// KDS システム - PWA JavaScript (Milestone 2)

// グローバル状態
const state = {
    page: 'order',
    ws: null,
    online: false,
    data: null, // API state cache
    cart: [], // 注文カート
    settingsTab: 'main' // 設定タブ (main|side|system|chinchiro)
};

// DOM要素
const app = document.getElementById('app');
const offlineModal = document.getElementById('offline-modal');
const reconnectBtn = document.getElementById('reconnect-btn');
const statusIndicator = document.getElementById('connection-status');

// 初期化
document.addEventListener('DOMContentLoaded', () => {
    console.log('KDS PWA 初期化中...');
    
    // Service Worker 登録
    if ('serviceWorker' in navigator) {
        navigator.serviceWorker.register('/sw.js')
            .then(reg => console.log('Service Worker 登録成功:', reg))
            .catch(err => console.error('Service Worker 登録失敗:', err));
    }
    
    // ナビゲーション設定
    setupNavigation();
    
    // WebSocket接続
    connectWs();
    
    // 再接続ボタン
    reconnectBtn.addEventListener('click', connectWs);
    
    // 初期データ取得
    loadStateData();
    
    // 時刻同期（起動時に1回）
    syncTimeOnce();
    
    // 定期的な時刻同期（30分毎）
    setInterval(syncTimeOnce, 30 * 60 * 1000);
    
    // グローバルエラーハンドリング設定
    window.addEventListener("error", e => console.error("GLOBAL ERR", e.error || e.message));
    window.addEventListener("unhandledrejection", e => console.error("PROMISE REJECTION", e.reason));
    
    // イベント委譲で動的DOMに対応
    document.addEventListener("click", (ev) => {
        const btn = ev.target.closest("[data-action='confirm-order']");
        if (!btn) return;
        
        ev.preventDefault();
        
        // 二重送信ガード
        if (btn.dataset.loading === "1") return;
        btn.dataset.loading = "1";
        
        submitOrder().catch(console.error).finally(() => {
            delete btn.dataset.loading;
        });
    });
    
    // 初期ページ表示
    render();
});

// データ取得
async function loadStateData() {
    try {
        const response = await fetch('/api/state');
        state.data = await response.json();
        console.log('状態データ取得完了:', state.data);
        render(); // 再描画
    } catch (error) {
        console.error('状態データ取得エラー:', error);
    }
}

// 時刻同期（起動時に1回）- デバッグ強化
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

// ナビゲーション設定
function setupNavigation() {
    const navBtns = document.querySelectorAll('.nav-btn');
    
    navBtns.forEach(btn => {
        btn.addEventListener('click', (e) => {
            const page = e.target.dataset.page;
            if (page) {
                // アクティブ状態更新
                navBtns.forEach(b => b.classList.remove('active'));
                e.target.classList.add('active');
                
                // ページ切り替え
                state.page = page;
                render();
            }
        });
    });
    
    // 初期アクティブ設定
    document.querySelector(`[data-page="${state.page}"]`).classList.add('active');
}

// WebSocket接続
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
        
        // 3秒後に自動再接続
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
                // スナップショット同期
                loadStateData();
            } else if (data.type === 'order.created' || data.type === 'order.updated') {
                // 注文更新
                loadStateData();
            } else if (data.type === 'printer.status') {
                // プリンタ状態更新
                if (state.data) {
                    state.data.printer.paperOut = data.paperOut !== undefined ? data.paperOut : state.data.printer.paperOut;
                    state.data.printer.holdJobs = data.holdJobs !== undefined ? data.holdJobs : state.data.printer.holdJobs;
                    render();
                }
            }
            
        } catch (err) {
            console.error('メッセージ解析エラー:', err);
        }
    };
}

// オンライン状態更新
function updateOnlineStatus(online) {
    state.online = online;
    
    statusIndicator.className = `status-indicator ${online ? 'online' : 'offline'}`;
    
    if (online) {
        offlineModal.classList.add('hidden');
    } else {
        offlineModal.classList.remove('hidden');
    }
}

// 紙切れモーダル表示・非表示
function updatePaperOutModal() {
    const modal = document.getElementById('paper-out-modal');
    if (state.data && state.data.printer.paperOut) {
        if (!modal) {
            // モーダル動的作成
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

// ページレンダリング
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
        default:
            content = '<div class="card"><h2>ページが見つかりません</h2></div>';
    }
    
    app.innerHTML = content;
    
    // 紙切れモーダル確認
    updatePaperOutModal();
    
    // ページ固有のイベントリスナー設定
    setupPageEvents();
}

// 注文ページ
function renderOrderPage() {
    if (!state.data) {
        return '<div class="card"><h2>📱 注文受付</h2><p>データ読込中...</p></div>';
    }
    
    const mainItems = state.data.menu.filter(item => item.category === 'MAIN' && item.active);
    const sideItems = state.data.menu.filter(item => item.category === 'SIDE' && item.active);
    const cookingOrders = state.data.orders.filter(order => order.status === 'COOKING');
    
    // 紙切れ時の警告
    const paperWarning = state.data.printer.paperOut ? 
        '<div class="card" style="border-left-color: #dc3545;"><h3>⚠️ 注文受付停止中</h3><p>プリンタ用紙切れのため、注文を受け付けできません。</p></div>' : '';
    
    return `
        ${paperWarning}
        
        <div class="card">
            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
                <h2>新規注文</h2>
                <button class="btn btn-info" onclick="toggleCompletedOrders()" id="toggle-completed-btn">
                    📋 注文済み一覧表示
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
                ${mainItems.map(item => `
                    <div class="card">
                        <h4>${item.name}</h4>
                        <p>通常: ${item.price_normal}円</p>
                        ${item.presale_discount_amount < 0 ? `<p>前売: ${item.price_normal + item.presale_discount_amount}円</p>` : ''}
                        <button class="btn btn-primary" onclick="addToCart('SET', '${item.sku}', 'normal')" ${state.data.printer.paperOut ? 'disabled' : ''}>
                            通常で追加
                        </button>
                        ${(state.data.settings.presaleEnabled && item.presale_discount_amount < 0) ? `
                        <button class="btn btn-success" onclick="addToCart('SET', '${item.sku}', 'presale')" ${state.data.printer.paperOut ? 'disabled' : ''}>
                            前売で追加
                        </button>` : ''}
                    </div>
                `).join('')}
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
                    <button class="btn btn-success btn-large" data-action="confirm-order" type="button" ${state.cart.length === 0 || state.data.printer.paperOut ? 'disabled' : ''} style="font-size: 1.5em; padding: 15px 30px; width: 100%; margin-bottom: 10px;">
                        📝 注文確定
                    </button>
                    <button class="btn btn-secondary" onclick="clearCart()" style="width: 100%;">
                        🗑️ カートクリア
                    </button>
                </div>
            </div>
        </div>
        

    `;
}

// キッチンページ
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
                                        <div class="item-name" style="font-size: 1.1em; font-weight: bold;">${item.name}</div>
                                        <div class="item-qty" style="font-size: 1.3em; color: #007bff; font-weight: bold;">
                                            数量: ${item.qty}個
                                        </div>
                                        <div class="item-price" style="font-size: 1em; color: #28a745; font-weight: bold;">
                                            単価: ${item.unitPriceApplied || item.unitPrice || 0}円
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
                '<div class="no-orders" style="text-align: center; font-size: 1.5em; color: #666; margin-top: 50px;">🎉 調理待ちの注文はありません</div>' : 
                ''}
        </div>
    `;
}

// 品出しページ
function renderPickupPage() {
    if (!state.data) {
        return '<div class="card"><h2>📦 品出し管理</h2><p>データ読込中...</p></div>';
    }
    
    // 品出し画面では調理中（COOKING）と調理完了（DONE）の商品を表示、品出し済み（READY）は非表示
    const pickupOrders = state.data.orders
        .filter(order => order.status === 'COOKING' || order.status === 'DONE')
        .sort((a, b) => b.ts - a.ts)
        .slice(0, 30);
    
    return `
        <div class="card">
            <h2>品出し画面 (${pickupOrders.length}件)</h2>
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
                        <div class="card order-card pickup-card" onclick="showOrderDetail('${order.orderNo}')" 
                             style="border-left-color: ${statusColor}; background-color: #f8f9fa;">
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
                            <div class="pickup-actions">
                                <button class="btn btn-info btn-sm" onclick="event.stopPropagation(); updateOrderStatus('${order.orderNo}', 'READY')">
                                    ✅ ${actionText}
                                </button>
                                <small style="display: block; margin-top: 5px;">クリックで詳細表示</small>
                            </div>
                        </div>
                    `;
                }).join('')}
            </div>
            ${pickupOrders.length === 0 ? '<p>🎉 品出し待ちの注文はありません</p>' : ''}
        </div>
    `;
}

// 設定ページ
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
        </nav>
    `;
    
    let tabContent = '';
    
    if (state.settingsTab === 'main') {
        const mainItems = state.data.menu.filter(item => item.category === 'MAIN');
        tabContent = `
            <div class="card">
                <h3>メイン商品管理</h3>
                <div id="main-items">
                    ${mainItems.map((item, idx) => `
                        <div style="border: 1px solid #ddd; padding: 10px; margin: 10px 0;">
                            <input type="text" placeholder="ID" value="${item.sku}" id="main-id-${idx}" readonly style="background: #f5f5f5;">
                            <input type="text" placeholder="商品名" value="${item.name}" id="main-name-${idx}">
                            <input type="text" placeholder="商品名（ローマ字）" value="${item.nameRomaji || ''}" id="main-name-romaji-${idx}">
                            <input type="number" placeholder="通常価格" value="${item.price_normal}" id="main-normal-${idx}">
                            <input type="number" placeholder="前売割引額" value="${item.presale_discount_amount}" id="main-discount-${idx}">
                            <label><input type="checkbox" ${item.active ? 'checked' : ''} id="main-active-${idx}"> 有効</label>
                        </div>
                    `).join('')}
                    <div style="border: 1px dashed #ddd; padding: 10px; margin: 10px 0;">
                        <input type="text" placeholder="ID (自動採番)" id="main-id-new" readonly style="background: #f5f5f5;">
                        <input type="text" placeholder="新商品名" id="main-name-new">
                        <input type="text" placeholder="新商品名（ローマ字）" id="main-name-romaji-new">
                        <input type="number" placeholder="通常価格" id="main-normal-new">
                        <input type="number" placeholder="前売割引額" id="main-discount-new" value="-100">
                        <label><input type="checkbox" checked id="main-active-new"> 有効</label>
                    </div>
                </div>
                <button class="btn btn-primary" onclick="saveMainProducts()">保存</button>
            </div>
        `;
    } else if (state.settingsTab === 'side') {
        const sideItems = state.data.menu.filter(item => item.category === 'SIDE');
        tabContent = `
            <div class="card">
                <h3>サイド商品管理</h3>
                <div id="side-items">
                    ${sideItems.map((item, idx) => `
                        <div style="border: 1px solid #ddd; padding: 10px; margin: 10px 0;">
                            <input type="text" placeholder="ID" value="${item.sku}" id="side-id-${idx}" readonly style="background: #f5f5f5;">
                            <input type="text" placeholder="商品名" value="${item.name}" id="side-name-${idx}">
                            <input type="text" placeholder="商品名（ローマ字）" value="${item.nameRomaji || ''}" id="side-name-romaji-${idx}">
                            <input type="number" placeholder="単品価格" value="${item.price_single}" id="side-single-${idx}">
                            <input type="number" placeholder="セット時価格" value="${item.price_as_side}" id="side-set-${idx}">
                            <label><input type="checkbox" ${item.active ? 'checked' : ''} id="side-active-${idx}"> 有効</label>
                        </div>
                    `).join('')}
                    <div style="border: 1px dashed #ddd; padding: 10px; margin: 10px 0;">
                        <input type="text" placeholder="ID (自動採番)" id="side-id-new" readonly style="background: #f5f5f5;">
                        <input type="text" placeholder="新商品名" id="side-name-new">
                        <input type="text" placeholder="新商品名（ローマ字）" id="side-name-romaji-new">
                        <input type="number" placeholder="単品価格" id="side-single-new">
                        <input type="number" placeholder="セット時価格" id="side-set-new">
                        <label><input type="checkbox" checked id="side-active-new"> 有効</label>
                    </div>
                </div>
                <button class="btn btn-primary" onclick="saveSideProducts()">保存</button>
            </div>
        `;
    } else if (state.settingsTab === 'system') {
        tabContent = `
            <div class="card">
                <h3>システム設定</h3>
                <div style="margin: 20px 0;">
                    <h4>前売り機能</h4>
                    <label style="display: block; margin: 10px 0;">
                        <input type="checkbox" ${state.data.settings.presaleEnabled ? 'checked' : ''} id="presale-enabled"> 
                        前売り機能を有効にする
                    </label>
                    <small style="color: #666;">無効にすると注文画面で前売りボタンが非表示になります</small>
                </div>
                
                <div style="margin: 20px 0;">
                    <h4>店舗情報</h4>
                    <label style="display: block; margin: 10px 0;">
                        店舗名:
                        <input type="text" value="${state.data.settings.store.name}" id="store-name" style="width: 200px;">
                    </label>
                    <label style="display: block; margin: 10px 0;">
                        店舗名（レシート印刷用ローマ字）:
                        <input type="text" value="${state.data.settings.store.nameRomaji || 'KDS BURGER'}" id="store-name-romaji" style="width: 200px;">
                    </label>
                    <small style="color: #666; display: block; margin-bottom: 10px;">レシート印刷時に使用される英語表記です</small>
                    <label style="display: block; margin: 10px 0;">
                        レジスターID:
                        <input type="text" value="${state.data.settings.store.registerId}" id="register-id" style="width: 200px;">
                    </label>
                </div>
                
                <div style="margin: 20px 0;">
                    <h4>注文番号設定</h4>
                    <label style="display: block; margin: 10px 0;">
                        最小番号:
                        <input type="number" value="${state.data.settings.numbering.min}" id="numbering-min" min="1" max="9999" style="width: 100px;">
                    </label>
                    <label style="display: block; margin: 10px 0;">
                        最大番号:
                        <input type="number" value="${state.data.settings.numbering.max}" id="numbering-max" min="1" max="9999" style="width: 100px;">
                    </label>
                </div>
                
                <button class="btn btn-primary" onclick="saveSystemSettings()">システム設定を保存</button>
                
                <div style="margin: 30px 0; padding: 20px; border: 2px solid #dc3545; border-radius: 5px; background: #fff5f5;">
                    <h4 style="color: #dc3545;">⚠️ 危険な操作</h4>
                    <p>システムを完全に初期化します。全ての注文データ、注文番号カウンタ、設定が削除されます。</p>
                    <button class="btn" style="background: #dc3545; color: white;" onclick="resetSystem()">🔄 システム完全初期化</button>
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'sales') {
        // 売上統計を計算
        const salesStats = calculateSalesStats();
        
        tabContent = `
            <div class="card">
                <h3>売上確認 - 現在のセッション</h3>
                
                <!-- 全体統計 -->
                <div class="sales-overview" style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0;">
                    <div class="stat-card" style="background: #e3f2fd; padding: 15px; border-radius: 8px; text-align: center;">
                        <h4 style="margin: 0 0 10px 0; color: #1976d2;">総注文数</h4>
                        <div style="font-size: 2em; font-weight: bold; color: #1976d2;">${salesStats.totalOrders}</div>
                        <small style="color: #666;">件</small>
                    </div>
                    <div class="stat-card" style="background: #e8f5e8; padding: 15px; border-radius: 8px; text-align: center;">
                        <h4 style="margin: 0 0 10px 0; color: #388e3c;">総売上</h4>
                        <div style="font-size: 2em; font-weight: bold; color: #388e3c;">¥${salesStats.totalRevenue.toLocaleString()}</div>
                        <small style="color: #666;">円</small>
                    </div>
                    <div class="stat-card" style="background: #fff3e0; padding: 15px; border-radius: 8px; text-align: center;">
                        <h4 style="margin: 0 0 10px 0; color: #f57c00;">平均単価</h4>
                        <div style="font-size: 2em; font-weight: bold; color: #f57c00;">¥${salesStats.averageOrder.toLocaleString()}</div>
                        <small style="color: #666;">円</small>
                    </div>
                    <div class="stat-card" style="background: #fce4ec; padding: 15px; border-radius: 8px; text-align: center;">
                        <h4 style="margin: 0 0 10px 0; color: #c2185b;">総商品数</h4>
                        <div style="font-size: 2em; font-weight: bold; color: #c2185b;">${salesStats.totalItems}</div>
                        <small style="color: #666;">個</small>
                    </div>
                </div>
                
                <!-- 商品別売上 -->
                <div style="margin: 30px 0;">
                    <h4>📈 商品別売上統計</h4>
                    <div class="sales-table" style="overflow-x: auto;">
                        <table style="width: 100%; border-collapse: collapse; margin-top: 15px;">
                            <thead>
                                <tr style="background: #f5f5f5;">
                                    <th style="padding: 10px; border: 1px solid #ddd; text-align: left;">商品名</th>
                                    <th style="padding: 10px; border: 1px solid #ddd; text-align: center;">販売数</th>
                                    <th style="padding: 10px; border: 1px solid #ddd; text-align: right;">売上金額</th>
                                    <th style="padding: 10px; border: 1px solid #ddd; text-align: right;">構成比</th>
                                </tr>
                            </thead>
                            <tbody>
                                ${salesStats.itemStats.map(item => `
                                    <tr>
                                        <td style="padding: 10px; border: 1px solid #ddd;">${item.name}</td>
                                        <td style="padding: 10px; border: 1px solid #ddd; text-align: center; font-weight: bold;">${item.quantity}</td>
                                        <td style="padding: 10px; border: 1px solid #ddd; text-align: right; font-weight: bold;">¥${item.revenue.toLocaleString()}</td>
                                        <td style="padding: 10px; border: 1px solid #ddd; text-align: right;">${item.percentage.toFixed(1)}%</td>
                                    </tr>
                                `).join('')}
                            </tbody>
                        </table>
                    </div>
                </div>
                
                <!-- 注文状況 -->
                <div style="margin: 30px 0;">
                    <h4>📋 注文状況内訳</h4>
                    <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-top: 15px;">
                        <div style="background: #fff9c4; padding: 10px; border-radius: 5px; text-align: center;">
                            <div style="font-weight: bold; color: #f57c00;">調理中</div>
                            <div style="font-size: 1.5em; margin: 5px 0;">${salesStats.statusCounts.COOKING || 0}</div>
                        </div>
                        <div style="background: #c8e6c9; padding: 10px; border-radius: 5px; text-align: center;">
                            <div style="font-weight: bold; color: #388e3c;">調理完了</div>
                            <div style="font-size: 1.5em; margin: 5px 0;">${salesStats.statusCounts.DONE || 0}</div>
                        </div>
                        <div style="background: #b3e5fc; padding: 10px; border-radius: 5px; text-align: center;">
                            <div style="font-weight: bold; color: #0277bd;">品出し完了</div>
                            <div style="font-size: 1.5em; margin: 5px 0;">${salesStats.statusCounts.READY || 0}</div>
                        </div>
                        <div style="background: #e1bee7; padding: 10px; border-radius: 5px; text-align: center;">
                            <div style="font-weight: bold; color: #7b1fa2;">提供済み</div>
                            <div style="font-size: 1.5em; margin: 5px 0;">${salesStats.statusCounts.DELIVERED || 0}</div>
                        </div>
                        <div style="background: #ffcdd2; padding: 10px; border-radius: 5px; text-align: center;">
                            <div style="font-weight: bold; color: #d32f2f;">キャンセル</div>
                            <div style="font-size: 1.5em; margin: 5px 0;">${salesStats.statusCounts.CANCELLED || 0}</div>
                        </div>
                    </div>
                </div>
                
                <!-- 更新ボタン -->
                <div style="text-align: center; margin-top: 30px;">
                    <button class="btn btn-primary" onclick="refreshSalesStats()" style="font-size: 1.1em; padding: 10px 20px;">
                        🔄 売上データ更新
                    </button>
                </div>
            </div>
        `;
    } else if (state.settingsTab === 'chinchiro') {
        tabContent = `
            <div class="card">
                <h3>ちんちろ設定</h3>
                <label>
                    <input type="checkbox" ${state.data.settings.chinchiro.enabled ? 'checked' : ''} id="chinchiro-enabled"> 
                    ちんちろ機能を有効にする
                </label>
                <p>倍率設定（カンマ区切り）:</p>
                <input type="text" value="${state.data.settings.chinchiro.multipliers.join(',')}" id="chinchiro-multipliers" style="width: 100%;">
                <p>丸め方式:</p>
                <select id="chinchiro-rounding">
                    <option value="round" ${state.data.settings.chinchiro.rounding === 'round' ? 'selected' : ''}>四捨五入</option>
                    <option value="floor" ${state.data.settings.chinchiro.rounding === 'floor' ? 'selected' : ''}>切り捨て</option>
                    <option value="ceil" ${state.data.settings.chinchiro.rounding === 'ceil' ? 'selected' : ''}>切り上げ</option>
                </select>
                <br><br>
                <button class="btn btn-primary" onclick="saveChinchoiroSettings()">保存</button>
            </div>
        `;
    }
    
    return tabNav + tabContent;
}

// エクスポートページ
function renderExportPage() {
    return `
        <div class="card">
            <h2>データエクスポート</h2>
            <div style="margin: 20px 0;">
                <button class="btn btn-primary" id="ping-test">🔗 接続テスト (/api/ping)</button>
                <button class="btn btn-success" onclick="downloadCsv()">📄 CSV エクスポート</button>
                <button class="btn btn-warning" onclick="restoreLatest()">🔄 1クリック復旧</button>
            </div>
            
            <div style="margin: 20px 0; padding: 15px; border: 2px solid #007bff; border-radius: 5px;">
                <h4>🖨️ ATOM Printerキット - 記事仕様テスト</h4>
                <p>Scrapbox記事に基づくATOM Printerキットの印刷テスト。TX=22,RX=19,9600bpsで動作します。</p>
                <button class="btn" style="background: #007bff; color: white; margin-right: 10px;" onclick="testNewPrintSystem()">🖨️ 新システム(記事仕様)</button>
                <button class="btn" style="background: #28a745; color: white; margin-right: 10px;" onclick="testPrintSelfCheck()">🔍 接続診断</button>
                <button class="btn" style="background: #ffc107; color: black;" onclick="testJapanesePrint()">🖨️ 確実印刷テスト</button>
            </div>
            
            <div id="api-result" style="margin-top: 20px;"></div>
        </div>
    `;
}

// ページ固有イベント設定
function setupPageEvents() {
    // カート更新
    updateCartDisplay();
    
    // 注文済み一覧が表示されている場合は更新
    const completedWidget = document.getElementById('completed-orders-widget');
    if (completedWidget && completedWidget.style.display !== 'none') {
        loadCompletedOrders();
    }
    
    // エクスポートページの ping テスト
    const pingBtn = document.getElementById('ping-test');
    if (pingBtn) {
        pingBtn.addEventListener('click', testPingApi);
    }
}

// カート操作（エラーハンドリング強化）
function addToCart(type, sku, priceMode = '') {
    try {
        // プリンター紙切れチェック
        if (state.data.printer.paperOut) {
            alert('プリンターの用紙を確認してください');
            return;
        }
        
        // ボタン連続クリック防止
        const button = event.target;
        if (button.disabled) return;
        button.disabled = true;
        
        if (type === 'SET') {
            // SETの場合、サイド選択ダイアログを表示（簡易実装：最初のサイド2つを自動選択）
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
            
            // 視覚的フィードバック
            button.style.backgroundColor = '#28a745';
            button.textContent = '追加完了!';
            
        } else if (type === 'SIDE_SINGLE') {
            state.cart.push({
                type: 'SIDE_SINGLE',
                sideSku: sku,
                qty: 1
            });
            
            // 視覚的フィードバック
            button.style.backgroundColor = '#28a745';
            button.textContent = '追加完了!';
        }
        
        updateCartDisplay();
        
        // ボタン復元（1秒後）
        setTimeout(() => {
            button.disabled = false;
            button.style.backgroundColor = '';
            button.textContent = button.textContent.includes('通常') ? '通常で追加' : 
                                button.textContent.includes('前売') ? '前売で追加' : '追加';
        }, 1000);
        
    } catch (error) {
        console.error('カート追加エラー:', error);
        alert('注文の追加に失敗しました。再試行してください。');
        // ボタン復元
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
    
    // デバッグ: カート内容を出力
    console.log('=== カート表示デバッグ ===');
    console.log('カート内容:', state.cart);
    console.log('メニューデータ数:', state.data.menu ? state.data.menu.length : 0);
    
    if (state.cart.length === 0) {
        cartDiv.innerHTML = '<p>カートは空です</p>';
        return;
    }
    
    let total = 0;
    const itemsHtml = state.cart.map((cartItem, idx) => {
        let itemTotal = 0;
        let description = '';
        
        if (cartItem.type === 'SET') {
            const mainItem = state.data.menu.find(item => item.sku === cartItem.mainSku);
            if (mainItem) {
                const mainPrice = cartItem.priceMode === 'presale' ? 
                    mainItem.price_normal + mainItem.presale_discount_amount : 
                    mainItem.price_normal;
                itemTotal += mainPrice;
                description = `${mainItem.name} (${cartItem.priceMode === 'presale' ? '前売' : '通常'})`;
                
                cartItem.sideSkus.forEach(sideSku => {
                    const sideItem = state.data.menu.find(item => item.sku === sideSku);
                    if (sideItem) {
                        itemTotal += sideItem.price_as_side;
                        description += ` + ${sideItem.name}`;
                    }
                });
            }
        } else if (cartItem.type === 'SIDE_SINGLE') {
            const sideItem = state.data.menu.find(item => item.sku === cartItem.sideSku);
            if (sideItem) {
                itemTotal = sideItem.price_single;
                description = `${sideItem.name} (単品)`;
            }
        }
        
        total += itemTotal * cartItem.qty;
        
        return `
            <div style="border: 1px solid #ddd; padding: 10px; margin: 5px 0;">
                <p>${description}</p>
                <p>数量: ${cartItem.qty} × ${itemTotal}円 = ${itemTotal * cartItem.qty}円</p>
                <button class="btn btn-secondary" onclick="removeFromCart(${idx})">削除</button>
            </div>
        `;
    }).join('');
    
    cartDiv.innerHTML = itemsHtml + `<p><strong>合計: ${total}円</strong></p>`;
}

function removeFromCart(index) {
    state.cart.splice(index, 1);
    updateCartDisplay();
}

// NaN耐性のある数値変換関数
function safeNum(v) { 
    const n = Number(v); 
    return Number.isFinite(n) ? n : 0; 
}

async function submitOrder() {
    console.log('=== submitOrder 呼び出し開始 ===');
    console.log('カート内容:', state.cart);
    console.log('カートサイズ:', state.cart.length);
    
    if (state.cart.length === 0) {
        alert('カートが空です');
        return;
    }
    
    // ボタン状態更新（data-action対応）
    const submitBtn = document.querySelector('[data-action="confirm-order"]');
    console.log('ボタン状態:', submitBtn ? `disabled=${submitBtn.disabled}` : 'ボタンが見つからない');
    if (submitBtn) {
        submitBtn.disabled = true;
        submitBtn.textContent = '⏳ 注文処理中...';
        console.log('ボタンをdisabledに設定');
    }
    
    // NaN耐性バリデーション - カートデータを安全化
    const safeCart = state.cart.map(item => {
        const safeItem = { ...item };
        if ('qty' in safeItem) safeItem.qty = Math.max(1, safeNum(safeItem.qty));
        if ('unitPriceApplied' in safeItem) safeItem.unitPriceApplied = safeNum(safeItem.unitPriceApplied);
        if ('unitPrice' in safeItem) safeItem.unitPrice = safeNum(safeItem.unitPrice);
        if ('discountValue' in safeItem) safeItem.discountValue = safeNum(safeItem.discountValue);
        return safeItem;
    });
    
    console.log('NaN耐性処理後のカート:', safeCart);
    
    // リトライ機能付きで注文送信
    const maxRetries = 3;
    let retryCount = 0;
    
    while (retryCount < maxRetries) {
        try {
            const response = await fetch('/api/orders', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ lines: safeCart }),
                timeout: 10000 // 10秒タイムアウト
            });
            
        if (response.ok) {
            const result = await response.json();
            
            // デバッグ: 注文送信内容と結果を出力
            console.log('=== 注文送信デバッグ ===');
            console.log('送信データ:', { lines: state.cart });
            console.log('サーバー応答:', result);
            
            // 成功時の処理
            clearCart();
            await loadStateData(); // 状態更新
            
            // 成功通知
            if (submitBtn) {
                submitBtn.style.backgroundColor = '#28a745';
                submitBtn.textContent = `✅ 注文確定: #${result.orderNo}`;
                
                // 2秒後にボタン復元
                setTimeout(() => {
                    submitBtn.disabled = false;
                    submitBtn.style.backgroundColor = '';
                    submitBtn.textContent = '📝 注文確定';
                }, 2000);
            }
            
            return; // 成功したのでリトライループを抜ける
            
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
                // リトライ前に1秒待機
                await new Promise(resolve => setTimeout(resolve, 1000));
                submitBtn.textContent = `再試行中... (${retryCount + 1}/${maxRetries})`;
            } else {
                // 最大リトライ数に達した場合
                alert(`注文の送信に失敗しました: ${error.message}\n\nカートの内容は保持されています。再度お試しください。`);
                
                // ボタン復元
                if (submitBtn) {
                    submitBtn.disabled = false;
                    submitBtn.style.backgroundColor = '#dc3545';
                    submitBtn.textContent = '📝 注文確定（再試行）';
                    
                    // 5秒後に通常状態に戻す
                    setTimeout(() => {
                        submitBtn.style.backgroundColor = '';
                        submitBtn.textContent = '📝 注文確定';
                    }, 5000);
                }
            }
        }
    }
}

// 注文操作
async function cancelOrder(orderNo) {
    const reason = prompt('キャンセル理由:') || '';
    
    try {
        const response = await fetch('/api/orders/cancel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `orderNo=${orderNo}&reason=${encodeURIComponent(reason)}`
        });
        
        if (response.ok) {
            alert(`注文 # ${orderNo} をキャンセルしました`);
            loadStateData();
        } else {
            alert('キャンセルに失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
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

// 設定操作
function switchSettingsTab(tab) {
    state.settingsTab = tab;
    render();
}

async function saveMainProducts() {
    const items = [];
    const mainItems = state.data.menu.filter(item => item.category === 'MAIN');
    
    // 既存アイテム
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
    
    // 新規アイテム
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
            loadStateData();
        } else {
            alert('保存に失敗しました');
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

// システム設定保存
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
            loadStateData(); // 設定を再読み込み
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
    
    // 既存アイテム
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
    
    // 新規アイテム
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
            loadStateData();
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

// エクスポート操作
async function downloadCsv() {
    try {
        // CSVエクスポート実行
        window.open('/api/export/csv', '_blank');
        
        // エクスポート完了後に営業セッション選択ダイアログを表示
        setTimeout(() => {
            showSessionEndDialog();
        }, 2000); // CSVダウンロード開始を待つ
        
    } catch (error) {
        console.error('CSVエクスポートエラー:', error);
        alert('CSVエクスポートに失敗しました');
    }
}

// 営業セッション終了選択ダイアログ
function showSessionEndDialog() {
    const modal = document.createElement('div');
    modal.className = 'modal-overlay';
    modal.innerHTML = `
        <div class="modal-content session-dialog">
            <h2>営業データのエクスポートが完了しました</h2>
            <p>今後の営業をどうしますか？</p>
            <div class="session-options">
                <button class="btn btn-success btn-large" onclick="continueSession()">
                    🔄 営業を続ける
                    <small>現在のデータをそのまま継続</small>
                </button>
                <button class="btn btn-warning btn-large" onclick="confirmEndSession()">
                    🏁 営業セッション終了
                    <small>データを初期化して新しいセッションを開始</small>
                </button>
            </div>
        </div>
    `;
    
    document.body.appendChild(modal);
    
    // モーダル外クリックでは閉じないように設定
    modal.addEventListener('click', (e) => {
        if (e.target === modal) {
            // 営業継続を選択したとみなす
            continueSession();
        }
    });
}

// 営業継続
function continueSession() {
    const modal = document.querySelector('.modal-overlay');
    if (modal) {
        document.body.removeChild(modal);
    }
    alert('営業を継続します。現在のデータが保持されます。');
}

// 営業セッション終了確認
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

// 営業セッション終了実行
async function endSession() {
    try {
        const response = await fetch('/api/session/end', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        });
        
        if (response.ok) {
            // モーダルを閉じる
            const modal = document.querySelector('.modal-overlay');
            if (modal) {
                document.body.removeChild(modal);
            }
            
            // 成功メッセージ
            alert('🎉 営業セッションが終了しました。\n新しいセッションを開始してください。');
            
            // データを再読み込みして新しいセッション登録画面を表示
            await loadStateData();
            state.page = 'order'; // 最初の画面に戻る
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
    if (!confirm('最新のスナップショットに復元しますか？')) return;
    
    try {
        const response = await fetch('/api/recover/restoreLatest', { method: 'POST' });
        const result = await response.json();
        
        if (result.ok) {
            alert(`復元完了: ${result.lastTs}`);
            loadStateData();
        } else {
            alert(`復元失敗: ${result.error}`);
        }
    } catch (error) {
        alert(`通信エラー: ${error.message}`);
    }
}

// API ping テスト
async function testPingApi() {
    const resultDiv = document.getElementById('api-result');
    resultDiv.innerHTML = '<p>⏳ 接続テスト中...</p>';
    
    try {
        const response = await fetch('/api/ping');
        const data = await response.json();
        
        resultDiv.innerHTML = `
            <div class="card" style="border-left-color: #28a745;">
                <h3>✅ 接続テスト成功</h3>
                <pre>${JSON.stringify(data, null, 2)}</pre>
            </div>
        `;
    } catch (error) {
        resultDiv.innerHTML = `
            <div class="card" style="border-left-color: #dc3545;">
                <h3>❌ 接続テスト失敗</h3>
                <p>エラー: ${error.message}</p>
            </div>
        `;
    }
}

// 注文詳細モーダル表示
function showOrderDetail(orderNo) {
    if (!state.data) return;
    
    const order = state.data.orders.find(o => o.orderNo === orderNo);
    if (!order) return;
    
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

// モーダルを閉じる
function closeModal() {
    const modal = document.querySelector('.modal-backdrop');
    if (modal) {
        modal.remove();
    }
}

// 状態に応じたアクションボタンを生成
function getStatusActions(order) {
    const actions = [];
    
    if (order.status === 'COOKING') {
        actions.push(`
            <button class="btn btn-success" onclick="updateOrderStatus('${order.orderNo}', 'DONE')">
                調理完了
            </button>
        `);
    }
    
    if (order.status === 'DONE') {
        actions.push(`
            <button class="btn btn-info" onclick="updateOrderStatus('${order.orderNo}', 'READY')">
                品出し完了
            </button>
        `);
    }
    
    return actions.length > 0 ? `
        <div class="modal-actions">
            ${actions.join('')}
        </div>
    ` : '';
}

// 状態ラベル
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

// 注文状態更新（新しいPATCH API使用）
async function updateOrderStatus(orderNo, newStatus) {
    try {
        const response = await fetch(`/api/orders/${orderNo}`, {
            method: 'PATCH',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ status: newStatus })
        });
        
        if (response.ok) {
            console.log(`注文 ${orderNo} を ${newStatus} に更新`);
            closeModal();
            // データ再取得
            await loadStateData();
        } else {
            alert('状態更新に失敗しました');
        }
    } catch (error) {
        console.error('状態更新エラー:', error);
        alert('状態更新に失敗しました');
    }
}

// 既存の completeOrder を新しい関数にリダイレクト（互換性保持）
function completeOrder(orderNo) {
    updateOrderStatus(orderNo, 'DONE');
}

// システム完全初期化
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
            
            // データ再読み込み
            await loadStateData();
            
            // カートもクリア
            cart = [];
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

// 日本語印刷テスト
async function testJapanesePrint() {
    const resultDiv = document.getElementById('api-result');
    
    if (!confirm('🖨️ 日本語印刷テストを実行します。\n\nサンプルレシートが印刷されます。\nプリンタの準備はよろしいですか？')) {
        return;
    }
    
    try {
        resultDiv.innerHTML = '<p>🖨️ 印刷テスト実行中...</p>';
        
        const response = await fetch('/api/print/test-jp', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        });
        
        if (response.ok) {
            const result = await response.json();
            resultDiv.innerHTML = `
                <div style="color: green; border: 1px solid green; padding: 10px; border-radius: 5px;">
                    <h4>✅ 印刷テスト成功</h4>
                    <p>${result.message}</p>
                    <p><strong>印刷内容:</strong></p>
                    <ul>
                        <li>店名: 九大料理サークルきゅう食 → Kyushoku Burger</li>
                        <li>注文番号: 55番 → Order No. 55</li>
                        <li>商品: 照り焼きバーガー x1 → Teriyaki Burger x1</li>
                        <li>商品: きゅう食バーガー x1 → Kyushoku Burger x1</li>
                        <li>合計: 1500円 → Total: 1500 yen</li>
                        <li>フッター: Thank you!</li>
                    </ul>
                </div>
            `;
        } else {
            const errorData = await response.json();
            resultDiv.innerHTML = `
                <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                    <h4>❌ 印刷テスト失敗</h4>
                    <p>${errorData.error || '不明なエラー'}</p>
                    <p>プリンタの接続と用紙を確認してください</p>
                </div>
            `;
        }
    } catch (error) {
        console.error('印刷テストエラー:', error);
        resultDiv.innerHTML = `
            <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                <h4>❌ 印刷テスト失敗</h4>
                <p>通信エラー: ${error.message}</p>
            </div>
        `;
    }
}

// 注文済み一覧の表示・非表示切り替え
function toggleCompletedOrders() {
    const widget = document.getElementById('completed-orders-widget');
    const button = document.getElementById('toggle-completed-btn');
    
    if (widget.style.display === 'none') {
        // 表示する
        widget.style.display = 'block';
        button.textContent = '📋 注文済み一覧非表示';
        loadCompletedOrders();
    } else {
        // 非表示にする
        widget.style.display = 'none';
        button.textContent = '📋 注文済み一覧表示';
    }
}

// 注文済み一覧を読み込み
function loadCompletedOrders() {
    if (!state.data) return;
    
    // 調理中、調理完了、品出し完了、提供済み、キャンセルされた注文を表示
    const completedOrders = state.data.orders
        .filter(order => ['COOKING', 'DONE', 'READY', 'DELIVERED', 'CANCELLED'].includes(order.status))
        .sort((a, b) => b.ts - a.ts)  // 新しい順
        .slice(0, 20);  // 最新20件
    
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
        
        const totalAmount = order.items.reduce((sum, item) => {
            const unitPrice = item.unitPriceApplied || item.unitPrice || 0;
            const qty = item.qty || 1;
            const discount = item.discountValue || 0;
            return sum + (unitPrice * qty - discount);
        }, 0);
        
        return `
            <div class="completed-order-item" style="border: 1px solid #ddd; margin: 10px 0; padding: 15px; border-radius: 5px;">
                <div class="order-header" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;">
                    <h4 style="margin: 0; color: #333;">注文 #${order.orderNo}</h4>
                    <span class="status-badge" style="background-color: ${statusColor}; color: white; padding: 4px 8px; border-radius: 3px; font-size: 0.8em;">
                        ${statusLabel}
                    </span>
                </div>
                <div class="order-info" style="font-size: 0.9em; color: #666; margin-bottom: 10px;">
                    <div>注文時刻: ${timeStr}</div>
                    <div>合計金額: ${totalAmount}円</div>
                </div>
                <div class="order-items" style="margin-bottom: 15px;">
                    ${order.items.slice(0, 3).map(item => 
                        `<span style="background: #f8f9fa; padding: 2px 6px; margin: 2px; border-radius: 3px; font-size: 0.8em; display: inline-block;">
                            ${item.name} x${item.qty}
                        </span>`
                    ).join('')}
                    ${order.items.length > 3 ? '<span style="color: #666;">...</span>' : ''}
                </div>
                <div class="order-actions" style="display: flex; gap: 10px; flex-wrap: wrap;">
                    <button class="btn btn-sm btn-info" onclick="showOrderDetail('${order.orderNo}')" style="font-size: 0.8em;">
                        📄 詳細
                    </button>
                    ${order.status !== 'CANCELLED' ? `
                    <button class="btn btn-sm btn-secondary" onclick="reprintReceipt('${order.orderNo}')" style="font-size: 0.8em;">
                        🖨️ 再印刷
                    </button>` : ''}
                    ${['COOKING', 'DONE'].includes(order.status) ? `
                    <button class="btn btn-sm btn-warning" onclick="cancelOrder('${order.orderNo}')" style="font-size: 0.8em;">
                        ❌ キャンセル
                    </button>` : ''}
                </div>
            </div>
        `;
    }).join('');
}

// 状態に応じた色を返す
function getStatusColor(status) {
    const colors = {
        'COOKING': '#ffc107',    // 黄色
        'DONE': '#28a745',       // 緑色
        'READY': '#17a2b8',      // 青色
        'DELIVERED': '#6c757d',  // グレー
        'CANCELLED': '#dc3545'   // 赤色
    };
    return colors[status] || '#6c757d';
}

// 売上統計計算
function calculateSalesStats() {
    if (!state.data || !state.data.orders) {
        return {
            totalOrders: 0,
            totalRevenue: 0,
            averageOrder: 0,
            totalItems: 0,
            itemStats: [],
            statusCounts: {}
        };
    }
    
    const orders = state.data.orders.filter(order => order.status !== 'CANCELLED');
    const itemMap = new Map();
    const statusCounts = {};
    
    let totalRevenue = 0;
    let totalItems = 0;
    
    // 各注文を処理
    orders.forEach(order => {
        // 状況カウント
        statusCounts[order.status] = (statusCounts[order.status] || 0) + 1;
        
        // 商品と売上を集計
        order.items.forEach(item => {
            if (item.kind === "ADJUST") return; // 調整行は除外
            
            const unitPrice = item.unitPriceApplied || item.unitPrice || 0;
            const qty = item.qty || 1;
            const discount = item.discountValue || 0;
            const itemTotal = (unitPrice * qty) - discount;
            
            totalRevenue += itemTotal;
            totalItems += qty;
            
            const itemName = item.name;
            if (itemMap.has(itemName)) {
                const existing = itemMap.get(itemName);
                existing.quantity += qty;
                existing.revenue += itemTotal;
            } else {
                itemMap.set(itemName, {
                    name: itemName,
                    quantity: qty,
                    revenue: itemTotal
                });
            }
        });
    });
    
    // 商品統計を配列に変換し、売上順にソート
    const itemStats = Array.from(itemMap.values())
        .sort((a, b) => b.revenue - a.revenue)
        .map(item => ({
            ...item,
            percentage: totalRevenue > 0 ? (item.revenue / totalRevenue) * 100 : 0
        }));
    
    const averageOrder = orders.length > 0 ? Math.round(totalRevenue / orders.length) : 0;
    
    return {
        totalOrders: orders.length,
        totalRevenue: totalRevenue,
        averageOrder: averageOrder,
        totalItems: totalItems,
        itemStats: itemStats,
        statusCounts: statusCounts
    };
}

// 売上統計更新
function refreshSalesStats() {
    if (state.settingsTab === 'sales') {
        render(); // 設定画面を再レンダリング
    }
}

// レシート再印刷機能
async function reprintReceipt(orderNo) {
    if (!confirm(`注文 #${orderNo} のレシートを再印刷しますか？`)) {
        return;
    }
    
    try {
        const response = await fetch('/api/orders/reprint', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ orderNo: orderNo })
        });
        
        if (response.ok) {
            const result = await response.json();
            alert(`✅ レシート再印刷を実行しました\n注文番号: ${orderNo}`);
        } else {
            const errorData = await response.json();
            alert(`❌ 再印刷に失敗しました: ${errorData.error || '不明なエラー'}`);
        }
    } catch (error) {
        console.error('再印刷エラー:', error);
        alert(`❌ 通信エラー: ${error.message}`);
    }
}

// T4. 新印刷システムテスト
async function testNewPrintSystem() {
    const resultDiv = document.getElementById('api-result');
    
    if (!confirm('🖨️ 新しい日本語印刷システムのテストを実行します。\n\nサンプルレシートが印刷されます。\nプリンタの準備はよろしいですか？')) {
        return;
    }
    
    try {
        resultDiv.innerHTML = '<p>🖨️ 新印刷システムテスト実行中...</p>';
        
        const response = await fetch('/api/print/test', {
            method: 'GET'
        });
        
        const result = await response.json();
        
        if (response.ok && result.ok) {
            resultDiv.innerHTML = `
                <div style="color: green; border: 1px solid green; padding: 10px; border-radius: 5px;">
                    <h4>✅ 新印刷システムテスト成功</h4>
                    <p>${result.message}</p>
                    <p><strong>印刷内容:</strong></p>
                    <ul>
                        <li>完全初期化: printerInit() 実行</li>
                        <li>英語店名: ビットマップレンダリング</li>
                        <li>注文番号: TEST-EN</li>
                        <li>商品: Teriyaki Burger, Kyushoku Burger (英語のみ)</li>
                        <li>自動フォールバック: GS v 0 → ESC * 対応</li>
                        <li>検査用黒バー付き</li>
                        <li>中国語文字化け完全回避</li>
                    </ul>
                </div>
            `;
        } else {
            resultDiv.innerHTML = `
                <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                    <h4>❌ 新印刷システムテスト失敗</h4>
                    <p>${result.error || '不明なエラー'}</p>
                    <p>プリンタの接続と電源を確認してください</p>
                </div>
            `;
        }
    } catch (error) {
        console.error('新印刷システムテストエラー:', error);
        resultDiv.innerHTML = `
            <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                <h4>❌ 新印刷システムテスト失敗</h4>
                <p>通信エラー: ${error.message}</p>
            </div>
        `;
    }
}

// T4. プリンタ自己診断テスト
async function testPrintSelfCheck() {
    const resultDiv = document.getElementById('api-result');
    
    if (!confirm('🔍 プリンタ自己診断を実行します。\n\n診断パターンが印刷されます。\nプリンタの準備はよろしいですか？')) {
        return;
    }
    
    try {
        resultDiv.innerHTML = '<p>🔍 プリンタ自己診断実行中...</p>';
        
        const response = await fetch('/api/print/selfcheck', {
            method: 'GET'
        });
        
        const result = await response.json();
        
        if (response.ok && result.ok) {
            resultDiv.innerHTML = `
                <div style="color: green; border: 1px solid green; padding: 10px; border-radius: 5px;">
                    <h4>✅ プリンタ自己診断成功</h4>
                    <p>${result.message}</p>
                    <p><strong>診断パターン:</strong></p>
                    <ul>
                        <li>黒バー (384×24ドット)</li>
                        <li>格子柄パターン</li>
                        <li>"SELF CHECK OK" テキスト</li>
                        <li>自動フォールバック機能確認</li>
                    </ul>
                </div>
            `;
        } else {
            resultDiv.innerHTML = `
                <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                    <h4>❌ プリンタ自己診断失敗</h4>
                    <p>${result.error || '不明なエラー'}</p>
                    <p>プリンタの接続、電源、用紙を確認してください</p>
                </div>
            `;
        }
    } catch (error) {
        console.error('プリンタ自己診断エラー:', error);
        resultDiv.innerHTML = `
            <div style="color: red; border: 1px solid red; padding: 10px; border-radius: 5px;">
                <h4>❌ プリンタ自己診断失敗</h4>
                <p>通信エラー: ${error.message}</p>
            </div>
        `;
    }
}