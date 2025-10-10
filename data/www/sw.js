const CACHE_NAME = 'kds-v7';
const STATIC_ASSETS = [
    '/',
    '/index.html',
    '/app.js',
    '/app.css',
    '/manifest.webmanifest'
];

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

self.addEventListener('fetch', (event) => {
    if (event.request.url.includes('/api/')) {
        event.respondWith(
            fetch(event.request)
                .catch(() => {
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
    
    if (event.request.url.includes('/ws')) {
        return;
    }
    
    event.respondWith(
        caches.match(event.request)
            .then((cachedResponse) => {
                if (cachedResponse) {
                    console.log('Service Worker: キャッシュから応答:', event.request.url);
                    return cachedResponse;
                }    
                return fetch(event.request)
                    .then((response) => {
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
                        if (event.request.mode === 'navigate') {
                            return caches.match('/index.html');
                        }
                    });
            })
    );
});