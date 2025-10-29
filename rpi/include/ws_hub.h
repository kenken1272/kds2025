#ifndef WS_HUB_H
#define WS_HUB_H

#include <ESPAsyncWebServer.h>

void initWsHub(AsyncWebServer &server);

nvoid wsBroadcast(const String &message);

#endif
