#include "server_routes.h"
#include "store.h"
#include "orders.h"
#include "printer_queue.h"
#include "csv_export.h"
#include "ws_hub.h"
#include "printer_render.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <cstdlib>

extern void requestAccessPointSuspend(uint32_t resumeDelayMs);
extern bool isAccessPointEnabled();
extern bool isAccessPointResumeScheduled();
extern uint32_t getAccessPointResumeEtaMs();

static void processReprintRequest(AsyncWebServerRequest *request, const JsonDocument& doc);
static void processCancelRequest(AsyncWebServerRequest *request, const uint8_t *data, size_t len);

static void fillOrderJson(JsonObject obj, const Order& order) {
  obj["orderNo"] = order.orderNo;
  obj["status"] = order.status;
  obj["ts"] = order.ts;
  obj["printed"] = order.printed;
  obj["cooked"] = order.cooked;
  obj["pickup_called"] = order.pickup_called;
  obj["picked_up"] = order.picked_up;
  if (!order.cancelReason.isEmpty()) {
    obj["cancelReason"] = order.cancelReason;
  }

n  JsonArray itemsArray = obj["items"].to<JsonArray>();
  for (const auto& item : order.items) {
    JsonObject j = itemsArray.add<JsonObject>();
    j["sku"] = item.sku;
    j["name"] = item.name;
    j["qty"] = item.qty;
    j["unitPriceApplied"] = item.unitPriceApplied;
    j["priceMode"] = item.priceMode;
    j["kind"] = item.kind;
    j["unitPrice"] = item.unitPrice;
    if (!item.discountName.isEmpty()) {
      j["discountName"] = item.discountName;
      j["discountValue"] = item.discountValue;
    }
  }
}

struct ArchiveStreamContext {
  AsyncResponseStream* stream;
  const String* sessionFilter;
  bool first;

  ArchiveStreamContext()
    : stream(nullptr), sessionFilter(nullptr), first(true) {}

n  ArchiveStreamContext(AsyncResponseStream* s, const String* filter, bool isFirst)
    : stream(s), sessionFilter(filter), first(isFirst) {}
};

static void processReprintRequest(AsyncWebServerRequest *request, const JsonDocument& doc) {
  String orderNo = doc["orderNo"] | "";
  Serial.printf("[API] 🖨️ 再印刷要求受信: '%s'\n", orderNo.c_str());

  if (orderNo.isEmpty()) {
    Serial.println("[API] ❌ エラー: 注文番号が空です");
    request->send(400, "application/json", "{\"error\":\"Missing orderNo in JSON body\"}");
    return;
  }

  Serial.printf("[API] 現在の注文数: %d件\n", S().orders.size());

  Order* active = nullptr;
  for (auto& o : S().orders) {
    if (o.orderNo == orderNo) {
      active = &o;
      break;
    }
  }

  Order archivedCopy;
  uint32_t archivedAtTs = 0;
  bool fromArchive = false;

  if (!active) {
    if (archiveFindOrder(S().session.sessionId, orderNo, archivedCopy, &archivedAtTs)) {
      Serial.printf("[API] ✅ アーカイブ注文発見: %s (archivedAt=%u)\n", orderNo.c_str(), archivedAtTs);
      fromArchive = true;
    }
  }

  if (!active && !fromArchive) {
    Serial.printf("[API] ❌ エラー: 注文番号 %s が見つかりません\n", orderNo.c_str());
    request->send(404, "application/json", "{\"error\":\"Order not found\"}");
    return;
  }

  const Order& target = fromArchive ? archivedCopy : *active;

  Serial.printf("[API] ✅ 注文発見: %s (status=%s, items=%d件, archived=%d)\n",
                target.orderNo.c_str(), target.status.c_str(), target.items.size(), fromArchive ? 1 : 0);

  if (target.status == "CANCELLED") {
    Serial.println("[API] ❌ エラー: キャンセル済み注文は再印刷不可");
    request->send(400, "application/json", "{\"error\":\"Cannot reprint cancelled order\"}");
    return;
  }

  if (target.items.empty()) {
    Serial.println("[API] ⚠️ エラー: 注文に明細がありません");
    request->send(400, "application/json", "{\"error\":\"Order has no items\"}");
    return;
  }

  Serial.printf("[API] 🖨️ レシート再印刷キュー追加: 注文番号 %s (items=%d, archived=%d)\n",
                orderNo.c_str(), target.items.size(), fromArchive ? 1 : 0);

  if (fromArchive) {
    enqueuePrint(archivedCopy);
  } else {
    enqueuePrint(*active);
  }

  JsonDocument res;
  res["ok"] = true;
  res["orderNo"] = orderNo;
  res["message"] = "Reprint job queued successfully";
  if (fromArchive) {
    res["archived"] = true;
    res["archivedAt"] = archivedAtTs;
  }

  String out; serializeJson(res, out);
  request->send(200, "application/json", out);
}

static String urlDecode(const String& value) {
  String result;
  result.reserve(value.length());
  for (int i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '+') {
      result += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char hex[3] = { value[i + 1], value[i + 2], '\0' };
      char decoded = static_cast<char>(strtol(hex, nullptr, 16));
      result += decoded;
      i += 2;
    } else {
      result += c;
    }
  }
  return result;
}

static void parseFormEncodedBody(const String& body, String& orderNo, String& reason) {
  int start = 0;
  while (start < body.length()) {
    int eq = body.indexOf('=', start);
    if (eq < 0) break;
    int amp = body.indexOf('&', eq + 1);
    String key = body.substring(start, eq);
    String val = (amp < 0) ? body.substring(eq + 1) : body.substring(eq + 1, amp);
    key = urlDecode(key);
    val = urlDecode(val);
    if (key == "orderNo") {
      orderNo = val;
    } else if (key == "reason") {
      reason = val;
    }
    if (amp < 0) {
      break;
    }
    start = amp + 1;
  }
}

static void processCancelRequest(AsyncWebServerRequest *request, const uint8_t *data, size_t len) {
  Serial.printf("[API] POST /api/orders/cancel (delegated) - len=%d\n", static_cast<int>(len));
  Serial.printf("  Content-Type: %s\n", request->contentType().c_str());

  String orderNo;
  String reason;
  String body(reinterpret_cast<const char*>(data), len);
  Serial.printf("  Raw body: %s\n", body.c_str());

  if (request->contentType().equalsIgnoreCase("application/json") || request->contentType().startsWith("application/json")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(data), len);
    if (err) {
      Serial.printf("[API] ❌ JSONデコード失敗: %s\n", err.c_str());
      request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    orderNo = String(static_cast<const char*>(doc["orderNo"] | ""));
    reason = String(static_cast<const char*>(doc["reason"] | ""));
  } else {
    parseFormEncodedBody(body, orderNo, reason);
  }

  Serial.printf("[API] キャンセル対象: 注文番号=%s, 理由=%s\n", orderNo.c_str(), reason.c_str());

  if (orderNo.isEmpty()) {
    Serial.println("[API] エラー: orderNoが取得できません");
    request->send(400, "application/json", "{\"error\":\"Missing orderNo parameter\"}");
    return;
  }

  Serial.printf("[API] 現在の注文数: %d件\n", S().orders.size());

  Order* activeOrder = nullptr;
  for (auto& o : S().orders) {
    if (o.orderNo == orderNo) {
      activeOrder = &o;
      break;
    }
  }

  Order archivedOrder;
  uint32_t archivedAtTs = 0;
  bool fromArchive = false;

  if (!activeOrder) {
    if (archiveFindOrder(S().session.sessionId, orderNo, archivedOrder, &archivedAtTs)) {
      fromArchive = true;
      activeOrder = &archivedOrder;
      Serial.printf("[API] ✅ アーカイブ注文発見: %s (archivedAt=%u)\n", orderNo.c_str(), archivedAtTs);
    }
  }

  if (!activeOrder) {
    Serial.printf("[API] ❌ エラー: 注文番号 %s が見つからない\n", orderNo.c_str());
    request->send(404, "application/json", "{\"error\":\"Order not found\"}");
    return;
  }

  if (activeOrder->status == "CANCELLED") {
    Serial.printf("[API] ⚠️ 既にキャンセル済み: %s\n", orderNo.c_str());
    request->send(400, "application/json", "{\"error\":\"Order already cancelled\"}");
    return;
  }

  Serial.printf("[API] ✅ 注文発見: %s (status=%s → CANCELLED)\n", activeOrder->orderNo.c_str(), activeOrder->status.c_str());
  activeOrder->status = "CANCELLED";
  activeOrder->cancelReason = reason;

  applyCancellationToSalesSummary(*activeOrder);

  bool requireSnapshot = !fromArchive;

  if (fromArchive) {
    if (!archiveReplaceOrder(*activeOrder, S().session.sessionId, archivedAtTs)) {
      Serial.printf("[API] ❌ アーカイブ更新失敗: %s\n", orderNo.c_str());
      request->send(500, "application/json", "{\"error\":\"Failed to update archived order\"}");
      return;
    }
  }

  StaticJsonDocument<768> walDoc;
  walDoc["ts"] = static_cast<uint32_t>(time(nullptr));
  walDoc["action"] = "ORDER_CANCEL";
  walDoc["orderNo"] = orderNo;
  walDoc["cancelReason"] = reason;
  if (fromArchive) {
    walDoc["archived"] = true;
  }
  String walLine; serializeJson(walDoc, walLine);
  walAppend(walLine);

  if (requireSnapshot) {
      requestSnapshotSave();
  }

  JsonDocument notify;
  notify["type"] = "order.updated";
  notify["orderNo"] = orderNo;
  notify["status"] = "CANCELLED";
  if (fromArchive) {
    notify["archived"] = true;
  }
  String msg; serializeJson(notify, msg);
  wsBroadcast(msg);

  Serial.printf("[API] ✅ キャンセル完了: 注文番号 %s (archived=%d)\n", orderNo.c_str(), fromArchive ? 1 : 0);
  JsonDocument res;
  res["ok"] = true;
  res["orderNo"] = orderNo;
  res["archived"] = fromArchive;
  if (fromArchive) {
    res["archivedAt"] = archivedAtTs;
  }
  String out; serializeJson(res, out);
  request->send(200, "application/json", out);
}

void initHttpRoutes(AsyncWebServer &server) {
  refreshMenuEtag();
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["ip"] = WiFi.softAPIP().toString();
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
    Serial.printf("API /ping 応答: %s\n", res.c_str());
  });

  // ... (rest of file copied verbatim)
  Serial.println("HTTP API ルート初期化完了");
}
