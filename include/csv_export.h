#pragma once
#include <ESPAsyncWebServer.h>

/**
 * CSVストリーミング出力を実行
 * 注文データを販売時点価格で1明細1行で出力
 * @param request HTTPリクエスト
 */
void sendCsvStream(AsyncWebServerRequest *request);