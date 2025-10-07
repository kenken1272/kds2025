// KDS Service Worker - PWA オフライン対応

const CACHE_NAME = 'kds-v2';
const STATIC_ASSETS = [
    '/',
    '/index.html',
    '/app.js',
    '/app.css',
    '/manifest.webmanifest'
];

// インストール時：静的アセットをキャッシュ
self.addEventListener('install', (event) => {
    console.log('Service Worker: インストール中...');
    
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then((cache) => {
                console.log('Service Worker: 静的アセットをキャッシュ');
                return cache.addAll(STATIC_ASSETS);
            })
            .then(() => {
                console.log('Service Worker: インストール完了');
                return self.skipWaiting();
            })
    );
});

// アクティベート時：古いキャッシュを削除
self.addEventListener('activate', (event) => {
    console.log('Service Worker: アクティベート中...');
    
    event.waitUntil(
        caches.keys()
            .then((cacheNames) => {
                return Promise.all(
                    cacheNames.map((cacheName) => {
                        if (cacheName !== CACHE_NAME) {
                            console.log('Service Worker: 古いキャッシュを削除:', cacheName);
                            return caches.delete(cacheName);
                        }
                    })
                );
            })
            .then(() => {
                console.log('Service Worker: アクティベート完了');
                return self.clients.claim();
            })
    );
});

// フェッチ時：Cache First 戦略
self.addEventListener('fetch', (event) => {
    // APIリクエストは常にネットワーク優先
    if (event.request.url.includes('/api/')) {
        event.respondWith(
            fetch(event.request)
                .catch(() => {
                    // API失敗時はエラーレスポンス
                    return new Response(
                        JSON.stringify({ error: 'Network unavailable' }), 
                        {
                            status: 503,
                            headers: { 'Content-Type': 'application/json' }
                        }
                    );
                })
        );
        return;
    }
    
    // WebSocket接続は素通し
    if (event.request.url.includes('/ws')) {
        return;
    }
    
    // 静的アセット：Cache First
    event.respondWith(
        caches.match(event.request)
            .then((cachedResponse) => {
                if (cachedResponse) {
                    console.log('Service Worker: キャッシュから応答:', event.request.url);
                    return cachedResponse;
                }
                
                // キャッシュにない場合はネットワークから取得
                return fetch(event.request)
                    .then((response) => {
                        // 成功した場合はキャッシュに保存
                        if (response.status === 200) {
                            const responseClone = response.clone();
                            caches.open(CACHE_NAME)
                                .then((cache) => {
                                    cache.put(event.request, responseClone);
                                });
                        }
                        return response;
                    })
                    .catch(() => {
                        // ネットワークエラー時はオフラインページ
                        if (event.request.mode === 'navigate') {
                            return caches.match('/index.html');
                        }
                    });
            })
    );
});