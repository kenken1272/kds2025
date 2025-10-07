#ifndef WS_HUB_H
#define WS_HUB_H

#include <ESPAsyncWebServer.h>

/**
 * WebSocket ハブの初期化
 * @param server WebServer インスタンス
 */
void initWsHub(AsyncWebServer &server);

/**
 * 全接続クライアントにメッセージをブロードキャスト
 * @param message 送信するメッセージ
 */
void wsBroadcast(const String &message);

#endif // WS_HUB_H