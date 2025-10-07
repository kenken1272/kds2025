#include "server_routes.h"
#include "store.h"
#include "orders.h"
#include "printer_queue.h"
#include "csv_export.h"
#include "ws_hub.h"
#include "printer_render.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>

void initHttpRoutes(AsyncWebServer &server) {
  // /api/ping
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["ip"] = WiFi.softAPIP().toString();
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
    Serial.printf("API /ping 応答: %s\n", res.c_str());
  });

  // /api/state
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;

    // settings
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

    // session
    doc["session"]["sessionId"] = S().session.sessionId;
    doc["session"]["startedAt"] = S().session.startedAt;
    doc["session"]["exported"]  = S().session.exported;

    // printer
    doc["printer"]["paperOut"]  = S().printer.paperOut;
    doc["printer"]["overheat"]  = S().printer.overheat;
    doc["printer"]["holdJobs"]  = S().printer.holdJobs;

    // menu
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

    // orders
    JsonArray ordersArray = doc["orders"].to<JsonArray>();
    for (const auto& od : S().orders) {
      JsonObject o = ordersArray.add<JsonObject>();
      o["orderNo"] = od.orderNo;
      o["status"]  = od.status;
      o["ts"]      = od.ts;
      o["printed"] = od.printed;

      JsonArray itemsArray = o["items"].to<JsonArray>();
      for (const auto& item : od.items) {
        JsonObject j = itemsArray.add<JsonObject>();
        j["sku"]  = item.sku;
        j["name"] = item.name;
        j["qty"]  = item.qty;
        j["unitPriceApplied"] = item.unitPriceApplied;
        j["priceMode"] = item.priceMode;
        j["kind"] = item.kind;
      }
    }

    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  // /api/products/main
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
      walAppend("MAIN_UPSERT," + String(millis()));
      snapshotSave();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  // /api/products/side
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
      walAppend("SIDE_UPSERT," + String(millis()));
      snapshotSave();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  // /api/settings/chinchiro
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

      walAppend("SETTINGS_UPDATE," + String(millis()));
      snapshotSave();

      JsonDocument sync; sync["type"] = "sync.snapshot";
      String msg; serializeJson(sync, msg); wsBroadcast(msg);

      request->send(200, "application/json", "{\"ok\":true}");
    });

  // /api/orders
  server.on("/api/orders", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (S().printer.paperOut) {
        request->send(503, "application/json", "{\"error\":\"Printer paper out\"}");
        return;
      }

      // 受信ログ
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

      // メニューチェック
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

      // stateに反映 → WAL → 印刷キュー → WS通知
      S().orders.push_back(order);
      walAppend("ORDER_CREATE," + order.orderNo);
      enqueuePrint(order);

      JsonDocument notify;
      notify["type"] = "order.created";
      notify["orderNo"] = order.orderNo;
      String msg; serializeJson(notify, msg); wsBroadcast(msg);

      JsonDocument resDoc; resDoc["orderNo"] = order.orderNo;
      String res; serializeJson(resDoc, res);
      request->send(200, "application/json", res);
    });

  // /api/orders/update
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

      walAppend("ORDER_UPDATE," + orderNo + "," + newStatus);
      snapshotSave();

      JsonDocument notify; notify["type"]="order.updated"; notify["orderNo"]=orderNo; notify["status"]=newStatus;
      String msg; serializeJson(notify, msg); wsBroadcast(msg);

      request->send(200, "application/json", "{\"ok\":true}");
    });

  // /api/orders/detail
  server.on("/api/orders/detail", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("orderNo")) {
      request->send(400, "application/json", "{\"error\":\"Missing orderNo parameter\"}");
      return;
    }
    String orderNo = request->getParam("orderNo")->value();

    Order* found = nullptr;
    for (auto& o : S().orders) if (o.orderNo == orderNo) { found = &o; break; }
    if (!found) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }

    JsonDocument res;
    res["orderNo"] = found->orderNo;
    res["status"]  = found->status;
    res["ts"]      = found->ts;
    res["printed"] = found->printed;

    JsonArray items = res["items"].to<JsonArray>();
    int total=0;
    for (const auto& it : found->items) {
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

  // /api/orders/cancel
  server.on("/api/orders/cancel", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("orderNo", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing orderNo\"}");
      return;
    }
    String orderNo = request->getParam("orderNo", true)->value();
    String reason = request->hasParam("reason", true) ? request->getParam("reason", true)->value() : "";

    bool found=false;
    for (auto& o : S().orders) if (o.orderNo == orderNo) { o.status="CANCELLED"; o.cancelReason=reason; found=true; break; }
    if (!found) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }

    walAppend("ORDER_CANCEL," + orderNo + "," + reason);
    snapshotSave();

    JsonDocument notify; notify["type"]="order.updated"; notify["orderNo"]=orderNo; notify["status"]="CANCELLED";
    String msg; serializeJson(notify, msg); wsBroadcast(msg);

    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/orders/reprint
  server.on("/api/orders/reprint", HTTP_POST, [](AsyncWebServerRequest *request){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, (char*)data, len)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      String orderNo = doc["orderNo"] | "";
      if (orderNo.isEmpty()) { request->send(400, "application/json", "{\"error\":\"Missing orderNo\"}"); return; }

      Order* found=nullptr;
      for (auto& o : S().orders) if (o.orderNo == orderNo) { found=&o; break; }
      if (!found) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }
      if (found->status == "CANCELLED") {
        request->send(400, "application/json", "{\"error\":\"Cannot reprint cancelled order\"}");
        return;
      }

      Serial.printf("レシート再印刷: 注文番号 %s\n", orderNo.c_str());
      enqueuePrint(*found);

      JsonDocument res; res["ok"]=true; res["orderNo"]=orderNo; res["message"]="Reprint job queued successfully";
      String out; serializeJson(res, out);
      request->send(200, "application/json", out);
    });

  // /api/printer/status
  server.on("/api/printer/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["paperOut"]   = S().printer.paperOut;
    doc["overheat"]   = S().printer.overheat;
    doc["holdJobs"]   = S().printer.holdJobs;
    doc["pendingJobs"]= getPendingPrintJobs();
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  // /api/printer/paper-replaced
  server.on("/api/printer/paper-replaced", HTTP_POST, [](AsyncWebServerRequest *request) {
    onPaperReplaced();

    JsonDocument notify;
    notify["type"] = "printer.status";
    notify["paperOut"] = S().printer.paperOut;
    notify["holdJobs"] = S().printer.holdJobs;
    String msg; serializeJson(notify, msg); wsBroadcast(msg);

    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/export/csv
  server.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendCsvStream(request);
  });

  // /api/recover/restoreLatest
  server.on("/api/recover/restoreLatest", HTTP_POST, [](AsyncWebServerRequest *request) {
    String lastTs; bool ok = recoverToLatest(lastTs);
    if (ok) {
      JsonDocument sync; sync["type"]="sync.snapshot"; String msg; serializeJson(sync, msg); wsBroadcast(msg);
      JsonDocument res; res["ok"]=true; res["lastTs"]=lastTs; String out; serializeJson(res, out);
      request->send(200, "application/json", out);
    } else {
      JsonDocument res; res["ok"]=false; res["error"]=lastTs; String out; serializeJson(res, out);
      request->send(500, "application/json", out);
    }
  });

  // PATCH /api/orders/:orderNo
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

      bool found=false;
      for (auto& o : S().orders) if (o.orderNo == orderNo) { o.status=newStatus; found=true; break; }
      if (!found) { request->send(404, "application/json", "{\"error\":\"Order not found\"}"); return; }

      walAppend("ORDER_UPDATE," + orderNo + "," + newStatus);
      snapshotSave();

      JsonDocument notify; notify["type"]="order.updated"; notify["orderNo"]=orderNo; notify["status"]=newStatus;
      String msg; serializeJson(notify, msg); wsBroadcast(msg);

      JsonDocument res; res["ok"]=true; String out; serializeJson(res, out);
      request->send(200, "application/json", out);
    });

  // POST /api/time/set
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

  // POST /api/settings/system
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

      snapshotSave();
      Serial.println("システム設定を保存しました");
      request->send(200, "application/json", "{\"ok\":true}");
    });

  // POST /api/session/end
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

    snapshotSave();
    walAppend("SESSION_END," + String(time(nullptr)));

    JsonDocument notify; notify["type"]="session.ended"; String msg; serializeJson(notify, msg); wsBroadcast(msg);

    request->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/system/reset
  server.on("/api/system/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("=== システム完全初期化開始 ===");

    Preferences prefs; prefs.begin("kds", false); prefs.clear(); prefs.end();
    Serial.println("NVS クリア完了");

    S().menu.clear(); S().orders.clear();
    S().session.sessionId = ""; S().session.startedAt = 0; S().session.exported = false;
    S().printer.paperOut = false; S().printer.overheat = false; S().printer.holdJobs = 0;

    ensureInitialMenu();
    if (snapshotSave()) Serial.println("スナップショット保存完了"); else Serial.println("警告: スナップショット保存失敗");

    JsonDocument notify; notify["type"]="system.reset"; String msg; serializeJson(notify, msg); wsBroadcast(msg);

    Serial.println("=== システム完全初期化完了 ===");
    request->send(200, "application/json", "{\"ok\":true,\"message\":\"システムを完全初期化しました\"}");
  });

  // ===== プリンタAPI（新実装に合わせて統一） =====

  // 日本語印刷テスト（POST/GETともにOK）
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

  // 総合テスト（printerInit + サンプル印字＝日本語ビットマップ）
  server.on("/api/print/test", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/test");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printJapaneseTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true,\"message\":\"Japanese test printing completed\"}":"{\"ok\":false,\"error\":\"Test printing failed\"}");
  });

  // 自己診断
  server.on("/api/print/selfcheck", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/selfcheck");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printSelfCheck();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true,\"message\":\"Self-check completed\"}":"{\"ok\":false,\"error\":\"Self-check failed\"}");
  });

  // ESC * 専用黒バー
  server.on("/api/print/selfcheck-escstar", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/selfcheck-escstar");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printSelfCheckEscStar();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  // ボーレート変更
  server.on("/api/print/baud", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/baud");
    String b = request->hasParam("b") ? request->getParam("b")->value() : "19200";
    int baud = b.toInt();
    if (baud != 9600 && baud != 19200) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"サポートされていないボーレートです (9600|19200)\"}");
      return;
    }
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    g_printerRenderer.updateBaudRate(baud);
    String msg = "{\"ok\":true,\"baud\":" + String(baud) + "}";
    request->send(200, "application/json", msg);
  });

  // 日本語直接印刷テスト
  server.on("/api/print/test-japanese", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/test-japanese");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printJapaneseTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  // 英語テスト印刷（ASCII直送）
  server.on("/api/print/test-english", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/test-english");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printEnglishTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  // 英語レシート（サンプル）→ 英語テスト印字に統一
  server.on("/api/print/receipt-english", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] GET /api/print/receipt-english");
    if (!g_printerRenderer.isReady()) { request->send(500, "application/json", "{\"ok\":false,\"error\":\"Printer not initialized\"}"); return; }
    bool ok = g_printerRenderer.printEnglishTest();
    request->send(ok?200:500, "application/json", ok? "{\"ok\":true}":"{\"ok\":false}");
  });

  // ================= Hello World 超ミニ疎通テスト =================
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
      "<p>HELLO WORLD を印刷して疎通確認します。<br>電源(12V/2.5A)・配線(RX=G23,TX=G33)・9600bps を確認してから押してください。</p>"
      "<button id='btn'>Print HELLO</button> <a href='/'>&larr; Home</a>"
      "<div id='log'></div>"
      "<script>const btn=document.getElementById('btn');const log=document.getElementById('log');btn.onclick=async()=>{btn.disabled=true;log.textContent='Requesting /api/print/hello ...\\n';try{const r=await fetch('/api/print/hello');const t=await r.text();log.textContent+='HTTP '+r.status+'\\n'+t;}catch(e){log.textContent+='ERROR: '+e;}btn.disabled=false;};</script>"
      "</body></html>";
    request->send(200, "text/html; charset=UTF-8", html);
  });

  Serial.println("HTTP API ルート初期化完了");
}