#include "ws_hub.h"
#include <ArduinoJson.h>

AsyncWebSocket ws("/ws");

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            JsonDocument doc;
            doc["type"] = "hello";
            doc["msg"] = "connected";
            String response;
            serializeJson(doc, response);
            client->text(response);
            break;
        }
        case WS_EVT_DISCONNECT:
        case WS_EVT_DATA:
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
        default:
            break;
    }
}

void initWsHub(AsyncWebServer &server) {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
}

void wsBroadcast(const String &message) {
    ws.textAll(message);
    String typeLabel = "?";
    if (!message.isEmpty()) {
        StaticJsonDocument<128> doc;
        if (!deserializeJson(doc, message)) {
            typeLabel = doc["type"].as<String>();
            if (typeLabel.isEmpty()) {
                typeLabel = "?";
            }
        }
    }
    Serial.printf("[WS] notify: %s\n", typeLabel.c_str());
}