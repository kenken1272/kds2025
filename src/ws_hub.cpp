#include "ws_hub.h"
#include <ArduinoJson.h>

AsyncWebSocket ws("/ws");

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    
    switch(type) {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket クライアント接続: %u\n", client->id());
            
            {
                JsonDocument doc;
                doc["type"] = "hello";
                doc["msg"] = "connected";
                String response;
                serializeJson(doc, response);
                client->text(response);
            }
            break;
            
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket クライアント切断: %u\n", client->id());
            break;
            
        case WS_EVT_DATA:
            Serial.printf("WebSocket データ受信: %u\n", client->id());
            break;
            
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            break;
    }
}

void initWsHub(AsyncWebServer &server) {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    Serial.println("WebSocket ハブ初期化完了 (/ws)");
}

void wsBroadcast(const String &message) {
    ws.textAll(message);
    Serial.printf("WebSocket ブロードキャスト: %s\n", message.c_str());
}