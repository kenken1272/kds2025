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

  JsonArray itemsArray = obj["items"].to<JsonArray>();
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

  ArchiveStreamContext(AsyncResponseStream* s, const String* filter, bool isFirst)
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
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["ip"] = WiFi.softAPIP().toString();
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
    Serial.printf("API /ping 応答: %s\n", res.c_str());
  });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool light = request->hasParam("light") && request->getParam("light")->value() == "1";
    size_t menuCount = S().menu.size();
    size_t orderCount = S().orders.size();
    if (light && orderCount > 60) {
      orderCount = 60;
    }
    size_t docCapacity = 8192 + menuCount * 384 + orderCount * 768;
    DynamicJsonDocument doc(docCapacity);
    doc["settings"]["catalogVersion"] = S().settings.catalogVersion;
    doc["settings"]["chinchiro"]["enabled"] = S().settings.chinchiro.enabled;
    JsonArray mult = doc["settings"]["chinchiro"]["multipliers"].to<JsonArray>();
    for (float m : S().settings.chinchiro.multipliers) mult.add(m);
    doc["settings"]["chinchiro"]["rounding"] = S().settings.chinchiro.rounding;
    doc["settings"]["store"]["name"] = S().settings.store.name;
    doc["settings"]["store"]["nameRomaji"] = S().settings.store.nameRomaji;
    doc["settings"]["store"]["registerId"] = S().settings.store.registerId;
    doc["settings"]["numbering"]["min"] = S().settings.numbering.min;
    doc["settings"]["numbering"]["max"] = S().settings.numbering.max;
    doc["settings"]["presaleEnabled"] = S().settings.presaleEnabled;
    doc["settings"]["qrPrint"]["enabled"] = S().settings.qrPrint.enabled;
    doc["settings"]["qrPrint"]["content"] = S().settings.qrPrint.content;

    doc["session"]["sessionId"] = S().session.sessionId;
    doc["session"]["startedAt"] = S().session.startedAt;
    doc["session"]["exported"]  = S().session.exported;

    doc["printer"]["paperOut"]  = S().printer.paperOut;
    doc["printer"]["overheat"]  = S().printer.overheat;
    doc["printer"]["holdJobs"]  = S().printer.holdJobs;
    JsonArray menuArray = doc["menu"].to<JsonArray>();
    for (const auto& it : S().menu) {
      JsonObject o = menuArray.add<JsonObject>();
      o["sku"]   = it.sku;
      o["name"]  = it.name;
      o["nameRomaji"] = it.nameRomaji;
      o["category"]   = it.category;
      o["active"]     = it.active;
      o["price_normal"] = it.price_normal;
      o["price_presale"] = it.price_presale;
      o["presale_discount_amount"] = it.presale_discount_amount;
      o["price_single"]   = it.price_single;
      o["price_as_side"]  = it.price_as_side;
    }

    JsonArray ordersArray = doc["orders"].to<JsonArray>();
    if (light) {
      size_t total = S().orders.size();
      size_t start = (total > 60) ? total - 60 : 0;
      for (size_t idx = start; idx < total; ++idx) {
        JsonObject o = ordersArray.add<JsonObject>();
        fillOrderJson(o, S().orders[idx]);
      }
    } else {
      for (const auto& od : S().orders) {
        JsonObject o = ordersArray.add<JsonObject>();
        fillOrderJson(o, od);
      }
    }

    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  server.on("/api/products/main", HTTP_POST, [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc["items"].is<JsonArray>()) {
        for (JsonVariantConst v : doc["items"].as<JsonArrayConst>()) {
          String id   = v["id"]   | "";
          String name = v["name"] | "";
          String nameRomaji = v["nameRomaji"] | "";
          int price_normal = v["price_normal"] | 0;
          int presale_discount_amount = v["presale_discount_amount"] | 0;
          bool active = v["active"] | true;

          if (id.isEmpty()) id = generateSkuMain();

          MenuItem* existing = nullptr;
          for (auto& it : S().menu) if (it.sku == id) { existing = &it; break; }

          if (existing) {
            existing->name = name;
            existing->nameRomaji = nameRomaji;
            existing->price_normal = price_normal;
            existing->presale_discount_amount = presale_discount_amount;
            existing->active = active;
          } else {
            MenuItem m;
            m.sku = id; m.name = name; m.nameRomaji = nameRomaji;
            m.category = "MAIN";
            m.price_normal = price_normal;
            m.presale_discount_amount = presale_discount_amount;
            m.active = active;
            S().menu.push_back(m);
          }
        }
      }
      // WAL記録（JSON形式）
      for (JsonVariantConst v : doc["items"].as<JsonArrayConst>()) {
  StaticJsonDocument<512> walDoc;
        walDoc["ts"] = (uint32_t)time(nullptr);
        walDoc["action"] = "MAIN_UPSERT";
        walDoc["sku"] = v["id"] | "";
        walDoc["name"] = v["name"] | "";
        walDoc["nameRomaji"] = v["nameRomaji"] | "";
        walDoc["price_normal"] = v["price_normal"] | 0;
        walDoc["presale_discount_amount"] = v["presale_discount_amount"] | 0;
        walDoc["active"] = v["active"] | true;
        String walLine; serializeJson(walDoc, walLine);
        walAppend(walLine);
      }
      requestSnapshotSave();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/products/side", HTTP_POST, [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc["items"].is<JsonArray>()) {
        for (JsonVariantConst v : doc["items"].as<JsonArrayConst>()) {
          String id   = v["id"]   | "";
          String name = v["name"] | "";
          String nameRomaji = v["nameRomaji"] | "";
          int price_single  = v["price_single"]  | 0;
          int price_as_side = v["price_as_side"] | 0;
          bool active       = v["active"] | true;

          if (id.isEmpty()) id = generateSkuSide();

          MenuItem* existing = nullptr;
          for (auto& it : S().menu) if (it.sku == id) { existing = &it; break; }

          if (existing) {
            existing->name = name;
            existing->nameRomaji = nameRomaji;
            existing->price_single = price_single;
            existing->price_as_side = price_as_side;
            existing->active = active;
          } else {
            MenuItem m;
            m.sku = id; m.name = name; m.nameRomaji = nameRomaji;
            m.category = "SIDE";
            m.price_single = price_single;
            m.price_as_side = price_as_side;
            m.active = active;
            S().menu.push_back(m);
          }
        }
      }
      // WAL記録（JSON形式）
      for (JsonVariantConst v : doc["items"].as<JsonArrayConst>()) {
  StaticJsonDocument<512> walDoc;
        walDoc["ts"] = (uint32_t)time(nullptr);
        walDoc["action"] = "SIDE_UPSERT";
        walDoc["sku"] = v["id"] | "";
        walDoc["name"] = v["name"] | "";
        walDoc["nameRomaji"] = v["nameRomaji"] | "";
        walDoc["price_single"] = v["price_single"] | 0;
        walDoc["price_as_side"] = v["price_as_side"] | 0;
        walDoc["active"] = v["active"] | true;
        String walLine; serializeJson(walDoc, walLine);
        walAppend(walLine);
      }
    requestSnapshotSave();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/settings/chinchiro", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      S().settings.chinchiro.enabled  = doc["enabled"]  | S().settings.chinchiro.enabled;
      S().settings.chinchiro.rounding = doc["rounding"] | S().settings.chinchiro.rounding;

      if (doc["multipliers"].is<JsonArray>()) {
        S().settings.chinchiro.multipliers.clear();
        for (JsonVariantConst v : doc["multipliers"].as<JsonArrayConst>()) {
          S().settings.chinchiro.multipliers.push_back(v.as<float>());
        }
      }

      // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
      walDoc["ts"] = (uint32_t)time(nullptr);
      walDoc["action"] = "SETTINGS_UPDATE";
      walDoc["chinchiro"]["enabled"] = S().settings.chinchiro.enabled;
      walDoc["chinchiro"]["rounding"] = S().settings.chinchiro.rounding;
      String walLine; serializeJson(walDoc, walLine);
      walAppend(walLine);
      
    requestSnapshotSave();

      JsonDocument sync; sync["type"] = "sync.snapshot";
      String msg; serializeJson(sync, msg); wsBroadcast(msg);

      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/settings/qrprint", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      S().settings.qrPrint.enabled = doc["enabled"] | S().settings.qrPrint.enabled;
      S().settings.qrPrint.content = doc["content"] | S().settings.qrPrint.content;

      Serial.printf("QR Print設定更新: enabled=%d, content=%s\n", 
                    S().settings.qrPrint.enabled, S().settings.qrPrint.content.c_str());

      // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
      walDoc["ts"] = (uint32_t)time(nullptr);
      walDoc["action"] = "SETTINGS_UPDATE";
      walDoc["qrPrint"]["enabled"] = S().settings.qrPrint.enabled;
      walDoc["qrPrint"]["content"] = S().settings.qrPrint.content;
      String walLine; serializeJson(walDoc, walLine);
      walAppend(walLine);
      
    requestSnapshotSave();

      JsonDocument sync; sync["type"] = "sync.snapshot";
      String msg; serializeJson(sync, msg); wsBroadcast(msg);

      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/orders", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      Serial.printf("[API] POST /api/orders - URL=%s\n", request->url().c_str());
      
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      const String urlPath = request->url();
      if (urlPath.endsWith("/reprint")) {
        processReprintRequest(request, doc);
        return;
      }
      if (urlPath.endsWith("/cancel")) {
        processCancelRequest(request, data, len);
        return;
      }

      if (S().printer.paperOut) {
        request->send(503, "application/json", "{\"error\":\"Printer paper out\"}");
        return;
      }

      Serial.println("=== 注文受信デバッグ ===");
      String in; serializeJson(doc, in); Serial.printf("受信JSON: %s\n", in.c_str());

      if (doc["lines"].is<JsonArray>()) {
        JsonArrayConst lines = doc["lines"].as<JsonArrayConst>();
        Serial.printf("lines配列サイズ: %d\n", lines.size());
        for (size_t i=0;i<lines.size();++i) {
          JsonVariantConst line = lines[i];
          const char* type = line["type"] | "不明";
          Serial.printf("  line[%d]: type=%s\n", (int)i, type);
          if (strcmp(type,"SET")==0) {
            const char* mainSku = line["mainSku"] | "不明";
            const char* priceMode = line["priceMode"] | "不明";
            int qty = line["qty"] | 0;
            Serial.printf("    mainSku=%s, priceMode=%s, qty=%d\n", mainSku, priceMode, qty);
            if (line["sideSkus"].is<JsonArray>()) {
              JsonArrayConst side = line["sideSkus"].as<JsonArrayConst>();
              Serial.printf("    sideSkus count=%d: ", side.size());
              for (size_t j=0;j<side.size();++j) {
                const char* s = side[j] | "不明"; Serial.printf("%s ", s);
              }
              Serial.println();
            }
          }
        }
      } else {
        Serial.println("警告: lines配列が存在しないか無効です");
      }

      if (S().menu.empty()) {
        Serial.println("緊急事態: メニューが空！初期メニュー投入");
        forceCreateInitialMenu();
        Serial.printf("投入完了: %d件\n", S().menu.size());
      } else {
        Serial.printf("メニューOK: %d件\n", S().menu.size());
        for (int i=0;i<S().menu.size();++i) {
          Serial.printf("  メニュー[%d]: SKU=%s, Name=%s, Category=%s\n",
            i, S().menu[i].sku.c_str(), S().menu[i].name.c_str(), S().menu[i].category.c_str());
        }
      }
      if (S().menu.empty()) {
        request->send(500, "application/json", "{\"error\":\"メニューデータが利用できません\"}");
        return;
      }

      Serial.println("=== buildOrderFromClientJson 呼び出し開始 ===");
      Order order = buildOrderFromClientJson(doc);
      Serial.println("=== buildOrderFromClientJson 呼び出し完了 ===");

      if (order.items.empty()) {
        Serial.println("エラー: 明細が空のため注文を拒否");
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"lines must be a non-empty array\"}");
        return;
      }

      Serial.println("=== 注文作成デバッグ ===");
      Serial.printf("注文番号: %s\n", order.orderNo.c_str());
      Serial.printf("アイテム数: %d\n", order.items.size());
      Serial.printf("状態: %s\n", order.status.c_str());
      for (int i=0;i<order.items.size();++i) {
        const auto& it = order.items[i];
        Serial.printf("アイテム%d: %s x%d (%d円) [%s]\n",
          i+1, it.name.c_str(), it.qty, it.unitPriceApplied, it.kind.c_str());
      }

  S().orders.push_back(order);
  applyOrderToSalesSummary(order);
      
    DynamicJsonDocument walDoc(4096);
      walDoc["ts"] = (uint32_t)time(nullptr);
      walDoc["action"] = "ORDER_CREATE";
    walDoc["orderNo"] = order.orderNo;
      orderToJson(walDoc.createNestedObject("order"), order);

      String walLine; serializeJson(walDoc, walLine);
      walAppend(walLine);
      
      enqueuePrint(order);

      if (!snapshotSave()) {
        Serial.println("[SNAP] save failed after order create");
        request->send(500, "application/json", R"({"error":"snapshotSave failed"})");
        return;
      }

      JsonDocument notify;
      notify["type"] = "order.created";
      notify["orderNo"] = order.orderNo;
      String msg; serializeJson(notify, msg); wsBroadcast(msg);

      JsonDocument resDoc; resDoc["orderNo"] = order.orderNo;
      String res; serializeJson(resDoc, res);
      request->send(200, "application/json", res);
    });

  server.on("/api/orders/update", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      String orderNo   = String((const char*)(doc["orderNo"] | ""));
      String newStatus = String((const char*)(doc["status"]  | ""));

      if (orderNo.isEmpty()) { request->send(400, "application/json", "{\"error\":\"Missing orderNo\"}"); return; }

      bool found=false;
      for (auto& o : S().orders) {
        if (o.orderNo == orderNo) {
          if (!newStatus.isEmpty()) o.status = newStatus;
          found=true; break;
        }
      }
      if (!found) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }

      // WAL記録（JSON形式）
      for (const auto& o : S().orders) {
        if (o.orderNo == orderNo) {
          StaticJsonDocument<512> walDoc;
          walDoc["ts"] = (uint32_t)time(nullptr);
          walDoc["action"] = "ORDER_UPDATE";
          walDoc["orderNo"] = orderNo;
          walDoc["status"] = newStatus;
          walDoc["cooked"] = o.cooked;
          walDoc["pickup_called"] = o.pickup_called;
          walDoc["picked_up"] = o.picked_up;
          walDoc["printed"] = o.printed;
          String walLine; serializeJson(walDoc, walLine);
          walAppend(walLine);
          break;
        }
      }
      
    requestSnapshotSave();

      JsonDocument notify; notify["type"]="order.updated"; notify["orderNo"]=orderNo; notify["status"]=newStatus;
      String msg; serializeJson(notify, msg); wsBroadcast(msg);

      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/orders/detail", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("orderNo")) {
      request->send(400, "application/json", "{\"error\":\"Missing orderNo parameter\"}");
      return;
    }
    String orderNo = request->getParam("orderNo")->value();

    Order orderData;
    bool found = false;
    for (auto& o : S().orders) {
      if (o.orderNo == orderNo) {
        orderData = o;
        found = true;
        break;
      }
    }

    if (!found) {
      if (!archiveFindOrder(S().session.sessionId, orderNo, orderData)) {
        request->send(404, "application/json", "{\"error\":\"Order not found\"}");
        return;
      }
    }

    JsonDocument res;
    res["orderNo"] = orderData.orderNo;
    res["status"]  = orderData.status;
    res["ts"]      = orderData.ts;
    res["printed"] = orderData.printed;

    JsonArray items = res["items"].to<JsonArray>();
    int total=0;
    for (const auto& it : orderData.items) {
      JsonObject j = items.add<JsonObject>();
      j["sku"]   = it.sku;
      j["name"]  = it.name;
      j["qty"]   = it.qty;
      j["unitPrice"] = it.unitPrice;
      j["unitPriceApplied"] = it.unitPriceApplied;
      j["priceMode"] = it.priceMode;
      j["kind"] = it.kind;
      j["discountValue"] = it.discountValue;

      int lineTotal = it.unitPriceApplied * it.qty - it.discountValue;
      j["lineTotal"] = lineTotal;
      total += lineTotal;
    }
    res["totalAmount"] = total;

    String out; serializeJson(res, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/orders/cancel", HTTP_POST, [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      processCancelRequest(request, data, len);
    });

  server.on("/api/sales/summary", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool rebuild = request->hasParam("rebuild");
    if (rebuild) {
      if (!recalculateSalesSummary()) {
        request->send(500, "application/json", "{\"error\":\"Failed to rebuild sales summary\"}");
        return;
      }
    }

    const SalesSummary& summary = getSalesSummary();
    DynamicJsonDocument doc(256);
    doc["sessionId"] = S().session.sessionId;
    doc["updatedAt"] = summary.lastUpdated;
    doc["confirmedOrders"] = summary.confirmedOrders;
    doc["cancelledOrders"] = summary.cancelledOrders;
    doc["totalOrders"] = summary.confirmedOrders + summary.cancelledOrders;
    doc["netSales"] = summary.revenue;
    doc["cancelledAmount"] = summary.cancelledAmount;
    doc["grossSales"] = summary.revenue + summary.cancelledAmount;
    doc["currency"] = "JPY";

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/printer/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["paperOut"]   = S().printer.paperOut;
    doc["overheat"]   = S().printer.overheat;
    doc["holdJobs"]   = S().printer.holdJobs;
    doc["pendingJobs"]= getPendingPrintJobs();
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  server.on("/api/printer/paper-replaced", HTTP_POST, [](AsyncWebServerRequest *request) {
    onPaperReplaced();

    JsonDocument notify;
    notify["type"] = "printer.status";
    notify["paperOut"] = S().printer.paperOut;
    notify["holdJobs"] = S().printer.holdJobs;
    String msg; serializeJson(notify, msg); wsBroadcast(msg);

    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/network/ap-cycle", HTTP_POST, [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> body;
      uint32_t resumeSec = 60;
      if (len > 0) {
        DeserializationError err = deserializeJson(body, data, len);
        if (!err) {
          if (body["resumeAfter"].is<uint32_t>()) {
            resumeSec = body["resumeAfter"].as<uint32_t>();
          } else if (body["resumeAfter"].is<int>()) {
            int raw = body["resumeAfter"].as<int>();
            if (raw > 0) {
              resumeSec = static_cast<uint32_t>(raw);
            }
          }
        }
      }

      if (resumeSec == 0) {
        resumeSec = 60;
      }

      requestAccessPointSuspend(resumeSec * 1000UL);
      Serial.printf("[WIFI] AP suspend requested for %u seconds\n", resumeSec);

      StaticJsonDocument<192> res;
      res["ok"] = true;
      res["apActive"] = isAccessPointEnabled();
      res["resumeInSec"] = resumeSec;
      if (isAccessPointResumeScheduled()) {
        res["resumeEtaMs"] = getAccessPointResumeEtaMs();
      }

      String out;
      serializeJson(res, out);
      request->send(200, "application/json", out);
    });

  server.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendCsvStream(request);
  });

  server.on("/api/export/sales-summary-lite", HTTP_GET, [](AsyncWebServerRequest *request) {
    const SalesSummary& summary = getSalesSummary();

    DynamicJsonDocument doc(320);
    doc["sessionId"] = S().session.sessionId;
    doc["generatedAt"] = static_cast<uint32_t>(time(nullptr));
    doc["lastUpdated"] = summary.lastUpdated;
    doc["confirmedOrders"] = summary.confirmedOrders;
    doc["cancelledOrders"] = summary.cancelledOrders;
    doc["totalOrders"] = summary.confirmedOrders + summary.cancelledOrders;
    doc["netSales"] = summary.revenue;
    doc["cancelledAmount"] = summary.cancelledAmount;
    doc["grossSales"] = summary.revenue + summary.cancelledAmount;
    doc["currency"] = "JPY";

    String out;
    serializeJson(doc, out);

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", out);
    response->addHeader("Content-Disposition", "attachment; filename=\"sales-summary-lite.json\"");
    request->send(response);
  });

  server.on("/api/export/snapshot", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json;
    String path;
    if (!getLatestSnapshotJson(json, path)) {
      request->send(404, "application/json", "{\"error\":\"snapshot not found\"}");
      return;
    }

    size_t baseSize = json.length();
    DynamicJsonDocument snapDoc(baseSize + 16384);
    DeserializationError err = deserializeJson(snapDoc, json);
    String filename = path.endsWith("snapA.json") ? "snapshotA.json" : "snapshotB.json";
    if (!err) {
      snapDoc["generatedAt"] = static_cast<uint32_t>(time(nullptr));
      String sessionId = S().session.sessionId;
      JsonArray archivedArray = snapDoc["archivedOrders"].to<JsonArray>();

      struct SnapshotArchiveContext {
        JsonArray* array;
        const String* sessionFilter;
      } ctx;
      ctx.array = &archivedArray;
      ctx.sessionFilter = &sessionId;

      auto visitor = [](const Order& order, const String& storedSession, uint32_t archivedAt, void* rawCtx) -> bool {
        auto* context = static_cast<SnapshotArchiveContext*>(rawCtx);
        if (!context || !context->array) {
          return false;
        }
        if (context->sessionFilter && !context->sessionFilter->isEmpty() && storedSession != *context->sessionFilter) {
          return true;
        }
        JsonObject obj = context->array->add<JsonObject>();
        fillOrderJson(obj, order);
        obj["archivedAt"] = archivedAt;
        return true;
      };

      archiveForEach(sessionId, visitor, &ctx);

      String out;
      serializeJson(snapDoc, out);
      AsyncWebServerResponse* response = request->beginResponse(200, "application/json", out);
      response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
      response->addHeader("X-Archive-Count", String(archivedArray.size()));
      request->send(response);
      return;
    }

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    request->send(response);
  });

  server.on("/api/orders/archive", HTTP_GET, [](AsyncWebServerRequest *request) {
    String sessionId = request->hasParam("sessionId") ? request->getParam("sessionId")->value() : S().session.sessionId;
    AsyncResponseStream* stream = request->beginResponseStream("application/json");
    stream->print('{');
    stream->print("\"sessionId\":\"");
    stream->print(sessionId);
    stream->print("\",\"orders\":[");

  ArchiveStreamContext ctx;
  ctx.stream = stream;
  ctx.sessionFilter = &sessionId;
  ctx.first = true;
    auto visitor = [](const Order& order, const String& storedSession, uint32_t archivedAt, void* rawCtx) -> bool {
      auto* context = static_cast<ArchiveStreamContext*>(rawCtx);
      if (!context || !context->stream) {
        return false;
      }
      if (context->sessionFilter && !context->sessionFilter->isEmpty() && storedSession != *context->sessionFilter) {
        return true;
      }
      DynamicJsonDocument orderDoc(estimateOrderDocumentCapacity(order) + 128);
      JsonObject obj = orderDoc.to<JsonObject>();
      fillOrderJson(obj, order);
      obj["archivedAt"] = archivedAt;
      String json;
      serializeJson(orderDoc, json);
      if (!context->first) {
        context->stream->print(',');
      }
      context->stream->print(json);
      context->first = false;
      return true;
    };

    archiveForEach(sessionId, visitor, &ctx);

    stream->print(']');
    stream->print('}');
    request->send(stream);
  });

  server.on("/api/system/memory", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["freeHeap"] = ESP.getFreeHeap();
#if defined(ESP32)
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
#endif
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  server.on("/api/recover", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("[API] POST /api/recover");
    
    String lastTs;
    bool ok = recoverToLatest(lastTs);
    
    if (ok) {
      Serial.printf("[API] 復旧成功: lastTs=%s\n", lastTs.c_str());
      
      // WebSocket通知を送信してフロントエンドを同期
      JsonDocument sync;
      sync["type"] = "sync.snapshot";
      String msg;
      serializeJson(sync, msg);
      wsBroadcast(msg);
      
      Serial.println("WebSocket ブロードキャスト: {\"type\":\"sync.snapshot\"}");
      
      // レスポンス
      JsonDocument res;
      res["ok"] = true;
      res["lastTs"] = lastTs;
      String out;
      serializeJson(res, out);
      request->send(200, "application/json", out);
    } else {
      Serial.printf("[API] 復旧失敗: error=%s\n", lastTs.c_str());
      JsonDocument res;
      res["ok"] = false;
      res["error"] = lastTs;
      String out;
      serializeJson(res, out);
      request->send(500, "application/json", out);
    }
  });

  server.on("^/api/orders/([0-9]{4})$", HTTP_PATCH, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      String orderNo   = request->pathArg(0);
      String newStatus = String((const char*)(doc["status"] | ""));
      if (newStatus.isEmpty()) { request->send(400, "application/json", "{\"error\":\"Missing status\"}"); return; }

      Serial.printf("[API] PATCH /api/orders/%s - status=%s (互換モード)\n", orderNo.c_str(), newStatus.c_str());

      Order* updatedOrder = nullptr;
      for (auto& o : S().orders) {
        if (o.orderNo == orderNo) {
          updatedOrder = &o;
          break;
        }
      }

      if (!updatedOrder) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }

      Order originalOrder = *updatedOrder;
      String notifyType = "order.updated";

      updatedOrder->status = newStatus;
      if (newStatus == "DONE" || newStatus == "COOKED") {
        updatedOrder->cooked = true;
        updatedOrder->pickup_called = true;
        notifyType = "order.cooked";
        Serial.printf("  → 互換処理: pickup_called=true (呼び出し画面に追加)\n");
      } else if (newStatus == "READY" || newStatus == "PICKED") {
        updatedOrder->picked_up = true;
        updatedOrder->pickup_called = false;
        notifyType = "order.picked";
        Serial.printf("  → 互換処理: pickup_called=false (呼び出し画面から削除)\n");
      }

      Order orderSnapshot = *updatedOrder;

      // WAL記録（JSON形式）
      StaticJsonDocument<512> walDoc;
      walDoc["ts"] = (uint32_t)time(nullptr);
      walDoc["action"] = "ORDER_UPDATE";
      walDoc["orderNo"] = orderNo;
      walDoc["status"] = newStatus;
      walDoc["cooked"] = orderSnapshot.cooked;
      walDoc["pickup_called"] = orderSnapshot.pickup_called;
      walDoc["picked_up"] = orderSnapshot.picked_up;
      walDoc["printed"] = orderSnapshot.printed;
      String walLine; serializeJson(walDoc, walLine);
      walAppend(walLine);

      bool shouldArchive = orderSnapshot.picked_up;
      if (shouldArchive) {
        if (!archiveOrderAndRemove(orderNo, S().session.sessionId)) {
          if (updatedOrder) {
            *updatedOrder = originalOrder;
          }
          request->send(500, "application/json", "{\"error\":\"Failed to archive order\"}");
          return;
        }
        updatedOrder = nullptr;
      }
      
    requestSnapshotSave();

      JsonDocument notify; 
      notify["type"] = notifyType;
      notify["orderNo"] = orderNo; 
      notify["status"] = newStatus;
      String msg; serializeJson(notify, msg); 
      wsBroadcast(msg);

      Serial.printf("  ✅ WebSocket通知送信: type=%s\n", notifyType.c_str());

      JsonDocument res; res["ok"]=true; String out; serializeJson(res, out);
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("^\\/api\\/orders\\/([0-9]+)\\/cooked$", HTTP_POST, [](AsyncWebServerRequest *request) {
    String path = request->url();
    Serial.printf("[API] POST リクエスト受信: %s\n", path.c_str());
    
    int startIdx = path.indexOf("/orders/") + 8;
    int endIdx = path.indexOf("/cooked");
    String orderNo = path.substring(startIdx, endIdx);
    
    Serial.printf("[API] 抽出された注文番号: %s\n", orderNo.c_str());
    
    bool found = false;
    for (auto& o : S().orders) {
      if (o.orderNo == orderNo) {
        o.cooked = true;
        o.pickup_called = true;
        found = true;
        Serial.printf("  ✅ 注文 %s を調理済みにマークしました\n", orderNo.c_str());
        break;
      }
    }
    if (!found) { 
      Serial.printf("  ❌ エラー: 注文 %s が見つかりません\n", orderNo.c_str());
      request->send(404, "application/json", "{\"error\":\"Order not found\"}"); 
      return; 
    }
    
    // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
    walDoc["ts"] = (uint32_t)time(nullptr);
    walDoc["action"] = "ORDER_COOKED";
    walDoc["orderNo"] = orderNo;
    String walLine; serializeJson(walDoc, walLine);
    walAppend(walLine);
    
    requestSnapshotSave();
    
    JsonDocument notify;
    notify["type"] = "order.cooked";
    notify["orderNo"] = orderNo;
    String msg; serializeJson(notify, msg);
    wsBroadcast(msg);
    
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("^\\/api\\/orders\\/([0-9]+)\\/picked$", HTTP_POST, [](AsyncWebServerRequest *request) {
    String path = request->url();
    Serial.printf("[API] POST リクエスト受信: %s\n", path.c_str());
    
    int startIdx = path.indexOf("/orders/") + 8;
    int endIdx = path.indexOf("/picked");
    String orderNo = path.substring(startIdx, endIdx);
    
    Serial.printf("[API] 抽出された注文番号: %s\n", orderNo.c_str());
    
    Order* targetOrder = nullptr;
    for (auto& o : S().orders) {
      if (o.orderNo == orderNo) {
        targetOrder = &o;
        break;
      }
    }
    if (!targetOrder) { 
      Serial.printf("  ❌ エラー: 注文 %s が見つかりません\n", orderNo.c_str());
      request->send(404, "application/json", "{\"error\":\"Order not found\"}"); 
      return; 
    }

    Order originalOrder = *targetOrder;
    targetOrder->picked_up = true;
    targetOrder->pickup_called = false;
    Serial.printf("  ✅ 注文 %s を品出し済みにマークしました\n", orderNo.c_str());
    
    // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
    walDoc["ts"] = (uint32_t)time(nullptr);
    walDoc["action"] = "ORDER_PICKED";
    walDoc["orderNo"] = orderNo;
    String walLine; serializeJson(walDoc, walLine);
    walAppend(walLine);

      if (!archiveOrderAndRemove(orderNo, S().session.sessionId)) {
        *targetOrder = originalOrder;
      request->send(500, "application/json", "{\"error\":\"Failed to archive order\"}");
      return;
    }
    
    requestSnapshotSave();
    
    JsonDocument notify;
    notify["type"] = "order.picked";
    notify["orderNo"] = orderNo;
    String msg; serializeJson(notify, msg);
    wsBroadcast(msg);
    
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/call-list", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray list = doc["callList"].to<JsonArray>();
    
    for (const auto& o : S().orders) {
      if (o.pickup_called) {
        JsonObject item = list.add<JsonObject>();
        item["orderNo"] = o.orderNo;
        item["ts"] = o.ts;
      }
    }
    
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  server.on("/api/time/set", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      time_t t = (time_t)(doc["epoch"].as<uint32_t>());
      struct timeval now = { .tv_sec = t, .tv_usec = 0 };
      settimeofday(&now, nullptr);
      setenv("TZ", "JST-9", 1); tzset();

      struct tm* ti = localtime(&t);
      char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S JST", ti);
      Serial.printf("時刻同期完了: %lu (%s)\n", (unsigned long)t, buf);
      request->send(200, "application/json", "{\"ok\":true}");
    });


  server.on("/api/settings/system", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc["presaleEnabled"].is<bool>()) S().settings.presaleEnabled = doc["presaleEnabled"].as<bool>();

      if (doc["store"].is<JsonObject>()) {
        if (doc["store"]["name"].is<const char*>())       S().settings.store.name = doc["store"]["name"].as<String>();
        if (doc["store"]["nameRomaji"].is<const char*>()) S().settings.store.nameRomaji = doc["store"]["nameRomaji"].as<String>();
        if (doc["store"]["registerId"].is<const char*>()) S().settings.store.registerId = doc["store"]["registerId"].as<String>();
      }

      if (doc["numbering"].is<JsonObject>()) {
        if (doc["numbering"]["min"].is<int>()) S().settings.numbering.min = doc["numbering"]["min"].as<uint16_t>();
        if (doc["numbering"]["max"].is<int>()) S().settings.numbering.max = doc["numbering"]["max"].as<uint16_t>();
      }

      requestSnapshotSave();
      Serial.println("システム設定を保存しました");
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/session/end", HTTP_POST, [](AsyncWebServerRequest *request) {
    S().orders.clear();
    S().session.exported = false;
    S().session.nextOrderSeq = 1;

    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char ds[32]; strftime(ds, sizeof(ds), "%Y-%m-%d-AM", ti);
    S().session.sessionId = String(ds);
    S().session.startedAt = now;

    S().printer.paperOut = false;
    S().printer.overheat = false;
    S().printer.holdJobs = 0;

    requestSnapshotSave();

    // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
    walDoc["ts"] = (uint32_t)time(nullptr);
    walDoc["action"] = "SESSION_END";
    String walLine; serializeJson(walDoc, walLine);
    walAppend(walLine);

    JsonDocument notify; notify["type"]="session.ended"; String msg; serializeJson(notify, msg); wsBroadcast(msg);

    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/system/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("=== システム完全初期化開始 ===");

    Preferences prefs; prefs.begin("kds", false); prefs.clear(); prefs.end();
    Serial.println("NVS クリア完了");

    S().menu.clear(); S().orders.clear();
    S().session.sessionId = ""; S().session.startedAt = 0; S().session.exported = false;
    S().printer.paperOut = false; S().printer.overheat = false; S().printer.holdJobs = 0;

    ensureInitialMenu();
    if (snapshotSave()) Serial.println("スナップショット保存完了"); else Serial.println("警告: スナップショット保存失敗");

    // WAL記録（JSON形式）
  StaticJsonDocument<512> walDoc;
    walDoc["ts"] = (uint32_t)time(nullptr);
    walDoc["action"] = "SYSTEM_RESET";
    String walLine; serializeJson(walDoc, walLine);
    walAppend(walLine);

    JsonDocument notify; notify["type"]="system.reset"; String msg; serializeJson(notify, msg); wsBroadcast(msg);

    Serial.println("=== システム完全初期化完了 ===");
    request->send(200, "application/json", "{\"ok\":true,\"message\":\"システムを完全初期化しました\"}");
  });

  server.on("/api/print/test-jp", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("=== 日本語印刷テスト開始 (POST) ===");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printJapaneseTest();
    request->send(ok?200:500, "application/json", ok?"{\"ok\":true}":"{\"ok\":false}");
  });
  server.on("/api/print/test-jp", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("=== 日本語印刷テスト開始 (GET) ===");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printJapaneseTest();
    String html = String(F("<!DOCTYPE html><meta charset='UTF-8'><title>印刷テスト</title>"))
      + (ok ? "<h1 style='color:green'>✅ 印刷テスト成功</h1>" : "<h1 style='color:red'>❌ 印刷テスト失敗</h1>")
      + "<p><a href='/'>← メインに戻る</a></p>";
    request->send(200, "text/html; charset=UTF-8", html);
  });

  server.on("/api/print/baud", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/baud");
    String b = request->hasParam("b") ? request->getParam("b")->value() : "115200"; 
    int baud = b.toInt();
    if (baud != 115200 && baud != 19200) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"サポートされていないボーレートです (115200|19200)\"}");
      return;
    }
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    g_printerRenderer.updateBaudRate(baud);
    String msg = "{\"ok\":true,\"baud\":" + String(baud) + "}";
    request->send(200, "application/json", msg);
  });

  server.on("/api/print/selfcheck-escstar", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/selfcheck-escstar");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printSelfCheckEscStar();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });


  server.on("/api/print/test-japanese", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/test-japanese");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printJapaneseTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });


  server.on("/api/print/test-english", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/test-english");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printEnglishTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/api/print/receipt-english", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/receipt-english");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printEnglishTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/api/print/hello", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("[API] GET /api/print/hello");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printHelloWorldTest();
    request->send(ok?200:500, "application/json", ok?"{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/debug/hello", HTTP_GET, [](AsyncWebServerRequest *request){
    String html =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<title>Printer Hello Test</title>"
      "<style>body{font-family:system-ui,Arial;margin:24px;}button{font-size:18px;padding:12px 20px;}#log{margin-top:16px;white-space:pre-wrap;border:1px solid #ccc;padding:12px;border-radius:8px;}a{color:#06c;text-decoration:none;}a:hover{text-decoration:underline;}</style>"
      "</head><body>"
      "<h1>Printer Hello Test</h1>"
      "<p>HELLO WORLD を印刷して疎通確認します。<br>電源(12V/2.5A)・配線(RX=G23,TX=G33)・115200bps を確認してから押してください。</p>"
      "<button id='btn'>Print HELLO</button> <a href='/'>&larr; Home</a>"
      "<div id='log'></div>"
      "<script>const btn=document.getElementById('btn');const log=document.getElementById('log');btn.onclick=async()=>{btn.disabled=true;log.textContent='Requesting /api/print/hello ...\\n';try{const r=await fetch('/api/print/hello');const t=await r.text();log.textContent+='HTTP '+r.status+'\\n'+t;}catch(e){log.textContent+='ERROR: '+e;}btn.disabled=false;};</script>"
      "</body></html>";
    request->send(200, "text/html; charset=UTF-8", html);
  });
  server.onNotFound([](AsyncWebServerRequest *request) {
    String method = (request->method() == HTTP_GET) ? "GET" : 
                   (request->method() == HTTP_POST) ? "POST" : 
                   (request->method() == HTTP_PUT) ? "PUT" : 
                   (request->method() == HTTP_DELETE) ? "DELETE" : "OTHER";
    Serial.printf("[404] %s %s\n", method.c_str(), request->url().c_str());
    if (request->url().indexOf("/api/orders/") >= 0) {
      Serial.println("  ⚠️ 注文関連APIがマッチしませんでした");
      Serial.printf("  URL: %s\n", request->url().c_str());
      Serial.printf("  Method: %s\n", method.c_str());
    }
    
    request->send(404, "application/json", "{\"error\":\"Not Found\"}");
  });

  Serial.println("HTTP API ルート初期化完了");
}