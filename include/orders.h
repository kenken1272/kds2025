#pragma once
#include "store.h"
#include <ArduinoJson.h>

/**
 * クライアントJSONから注文を構築
 * 販売時点価格スナップショット + ちんちろ調整を適用
 * @param req 注文JSON（lines配列）
 * @return 構築された注文オブジェクト
 */
Order buildOrderFromClientJson(const JsonDocument& req);

/**
 * ちんちろ調整額を計算
 * @param setSubtotal SET行の小計
 * @param multiplier 適用倍率
 * @param rounding 丸め方式 ("round"|"floor"|"ceil")
 * @return 調整額（±）
 */
int calculateChinchoiroAdjustment(int setSubtotal, float multiplier, const String& rounding);