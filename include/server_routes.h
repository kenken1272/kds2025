#ifndef SERVER_ROUTES_H
#define SERVER_ROUTES_H

#include <ESPAsyncWebServer.h>

/**
 * HTTPルートを初期化
 * 現在は /api/ping のみ実装
 * 将来のAPI拡張ポイント：
 * - /api/products/main, /api/products/side
 * - /api/orders
 * - /api/export/csv
 * - /api/printer/status, /api/printer/paper-replaced
 * - /api/recover/restoreLatest
 * 
 * @param server WebServer インスタンス
 */
void initHttpRoutes(AsyncWebServer &server);

#endif // SERVER_ROUTES_H