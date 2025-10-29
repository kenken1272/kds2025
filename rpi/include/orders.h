#pragma once
#include "store.h"
#include <ArduinoJson.h>

Order buildOrderFromClientJson(const JsonDocument& req);

nint calculateChinchoiroAdjustment(int setSubtotal, float multiplier, const String& rounding);
