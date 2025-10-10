#include "store.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

// グローバル状態
static State g_state;

State& S() {
    return g_state;
}

// NVS採番用
static Preferences prefs;

String allocateOrderNo() {
    prefs.begin("kds", false);
    uint16_t seq = prefs.getUShort("nextSeq", 1);
    
    // 衝突回避（最大100回ループ）
    for (int i = 0; i < 100; i++) {
        String candidate = String(seq);
        while (candidate.length() < 4) {
            candidate = "0" + candidate;
        }
        
        // 既存注文に同じ番号がないかチェック
        bool exists = false;
        for (const auto& order : S().orders) {
            if (order.orderNo == candidate) {
                exists = true;
                break;
            }
        }
        
        if (!exists) {
            // 次回用にインクリメント
            seq++;
            if (seq > 9999) seq = 1;
            prefs.putUShort("nextSeq", seq);
            prefs.end();
            return candidate;
        }
        
        seq++;
        if (seq > 9999) seq = 1;
    }
    
    prefs.end();
    // フォールバック
    return "9999";
}

String generateSkuMain() {
    prefs.begin("kds", false);
    uint16_t seq = prefs.getUShort("mainSeq", 1);
    
    String sku = "main_" + String(seq);
    while (sku.length() < 9) {
        sku = sku.substring(0, 5) + "0" + sku.substring(5);
    }
    
    seq++;
    prefs.putUShort("mainSeq", seq);
    prefs.end();
    
    return sku;
}

String generateSkuSide() {
    prefs.begin("kds", false);
    uint16_t seq = prefs.getUShort("sideSeq", 1);
    
    String sku = "side_" + String(seq);
    while (sku.length() < 9) {
        sku = sku.substring(0, 5) + "0" + sku.substring(5);
    }
    
    seq++;
    prefs.putUShort("sideSeq", seq);
    prefs.end();
    
    return sku;
}

// A/B交互保存の実装
static bool currentSnapshotIsA = true;

bool snapshotSave() {
    Serial.printf("=== snapshotSave開始: 注文数=%d, メニュー数=%d ===\n", 
                  S().orders.size(), S().menu.size());
    
    // ディレクトリ作成（存在しない場合）
    if (!LittleFS.exists("/kds")) {
        if (!LittleFS.mkdir("/kds")) {
            Serial.println("ディレクトリ作成失敗: /kds");
            return false;
        }
        Serial.println("ディレクトリ作成完了: /kds");
    }
    
    JsonDocument doc;
    
    // Settings
    doc["settings"]["catalogVersion"] = S().settings.catalogVersion;
    doc["settings"]["chinchiro"]["enabled"] = S().settings.chinchiro.enabled;
    JsonArray mult = doc["settings"]["chinchiro"]["multipliers"].to<JsonArray>();
    for (float m : S().settings.chinchiro.multipliers) {
        mult.add(m);
    }
    doc["settings"]["chinchiro"]["rounding"] = S().settings.chinchiro.rounding;
    doc["settings"]["numbering"]["min"] = S().settings.numbering.min;
    doc["settings"]["numbering"]["max"] = S().settings.numbering.max;
    doc["settings"]["store"]["name"] = S().settings.store.name;
    doc["settings"]["store"]["nameRomaji"] = S().settings.store.nameRomaji;
    doc["settings"]["store"]["registerId"] = S().settings.store.registerId;
    doc["settings"]["qrPrint"]["enabled"] = S().settings.qrPrint.enabled;
    doc["settings"]["qrPrint"]["content"] = S().settings.qrPrint.content;
    
    // Session
    doc["session"]["sessionId"] = S().session.sessionId;
    doc["session"]["startedAt"] = S().session.startedAt;
    doc["session"]["exported"] = S().session.exported;
    doc["session"]["nextOrderSeq"] = S().session.nextOrderSeq;
    
    // Printer
    doc["printer"]["paperOut"] = S().printer.paperOut;
    doc["printer"]["overheat"] = S().printer.overheat;
    doc["printer"]["holdJobs"] = S().printer.holdJobs;
    
    // Menu
    JsonArray menuArray = doc["menu"].to<JsonArray>();
    for (const auto& item : S().menu) {
        JsonObject menuItem = menuArray.add<JsonObject>();
        menuItem["sku"] = item.sku;
        menuItem["name"] = item.name;
        menuItem["nameRomaji"] = item.nameRomaji;
        menuItem["category"] = item.category;
        menuItem["active"] = item.active;
        menuItem["price_normal"] = item.price_normal;
        menuItem["price_presale"] = item.price_presale;
        menuItem["presale_discount_amount"] = item.presale_discount_amount;
        menuItem["price_single"] = item.price_single;
        menuItem["price_as_side"] = item.price_as_side;
    }
    
    // Orders
    JsonArray ordersArray = doc["orders"].to<JsonArray>();
    for (const auto& order : S().orders) {
        JsonObject orderObj = ordersArray.add<JsonObject>();
        orderObj["orderNo"] = order.orderNo;
        orderObj["status"] = order.status;
        orderObj["ts"] = order.ts;
        orderObj["printed"] = order.printed;
        orderObj["cancelReason"] = order.cancelReason;
        // 追加: 呼び出し画面関連フラグを保存
        orderObj["cooked"] = order.cooked;
        orderObj["picked_up"] = order.picked_up;
        orderObj["pickup_called"] = order.pickup_called;
        
        Serial.printf("  注文 %s: status=%s, cooked=%d, picked_up=%d, pickup_called=%d, items=%d件\n",
                      order.orderNo.c_str(), order.status.c_str(), 
                      order.cooked, order.picked_up, order.pickup_called, order.items.size());
        
        JsonArray itemsArray = orderObj["items"].to<JsonArray>();
        for (const auto& item : order.items) {
            JsonObject itemObj = itemsArray.add<JsonObject>();
            itemObj["sku"] = item.sku;
            itemObj["name"] = item.name;
            itemObj["qty"] = item.qty;
            itemObj["unitPriceApplied"] = item.unitPriceApplied;
            itemObj["priceMode"] = item.priceMode;
            itemObj["kind"] = item.kind;
            itemObj["unitPrice"] = item.unitPrice;
            itemObj["discountName"] = item.discountName;
            itemObj["discountValue"] = item.discountValue;
        }
    }
    
    // ファイル保存
    String filename = currentSnapshotIsA ? "/kds/snapA.json" : "/kds/snapB.json";
    File file = LittleFS.open(filename, "w");
    if (!file) {
        Serial.printf("スナップショット保存失敗: %s\n", filename.c_str());
        return false;
    }
    
    serializeJson(doc, file);
    file.flush();
    file.close();
    
    // 次回は反対のファイル
    currentSnapshotIsA = !currentSnapshotIsA;
    
    Serial.printf("スナップショット保存完了: %s\n", filename.c_str());
    return true;
}

bool snapshotLoad() {
    // A/B両方のタイムスタンプをチェック
    File fileA = LittleFS.open("/kds/snapA.json", "r");
    File fileB = LittleFS.open("/kds/snapB.json", "r");
    
    bool useA = true;
    if (fileA && fileB) {
        // 両方ある場合は新しい方を使用
        time_t timeA = fileA.getLastWrite();
        time_t timeB = fileB.getLastWrite();
        useA = (timeA >= timeB);
    } else if (fileB && !fileA) {
        useA = false;
    } else if (!fileA && !fileB) {
        // 両方ない場合は初期化
        fileA.close();
        fileB.close();
        ensureInitialMenu();
        return true;
    }
    
    File file = useA ? fileA : fileB;
    String filename = useA ? "/kds/snapA.json" : "/kds/snapB.json";
    
    if (!useA) {
        fileA.close();
    } else {
        fileB.close();
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("スナップショット読込エラー: %s - %s\n", filename.c_str(), error.c_str());
        ensureInitialMenu();
        return false;
    }
    
    // データ復元
    if (doc["settings"].is<JsonObject>()) {
        S().settings.catalogVersion = doc["settings"]["catalogVersion"] | 1;
        S().settings.chinchiro.enabled = doc["settings"]["chinchiro"]["enabled"] | true;
        S().settings.chinchiro.rounding = doc["settings"]["chinchiro"]["rounding"] | "round";
        
        S().settings.chinchiro.multipliers.clear();
        if (doc["settings"]["chinchiro"]["multipliers"].is<JsonArray>()) {
            for (JsonVariantConst v : doc["settings"]["chinchiro"]["multipliers"].as<JsonArrayConst>()) {
                S().settings.chinchiro.multipliers.push_back(v.as<float>());
            }
        }
        
        S().settings.numbering.min = doc["settings"]["numbering"]["min"] | 1;
        S().settings.numbering.max = doc["settings"]["numbering"]["max"] | 9999;
        S().settings.store.name = doc["settings"]["store"]["name"] | "KDS BURGER";
        S().settings.store.nameRomaji = doc["settings"]["store"]["nameRomaji"] | "KDS BURGER";
        S().settings.store.registerId = doc["settings"]["store"]["registerId"] | "REG-01";
        S().settings.qrPrint.enabled = doc["settings"]["qrPrint"]["enabled"] | false;
        S().settings.qrPrint.content = doc["settings"]["qrPrint"]["content"] | "";
    }
    
    if (doc["session"].is<JsonObject>()) {
        S().session.sessionId = doc["session"]["sessionId"] | "";
        S().session.startedAt = doc["session"]["startedAt"] | 0;
        S().session.exported = doc["session"]["exported"] | false;
        S().session.nextOrderSeq = doc["session"]["nextOrderSeq"] | 1;
    }
    
    if (doc["printer"].is<JsonObject>()) {
        S().printer.paperOut = doc["printer"]["paperOut"] | false;
        S().printer.overheat = doc["printer"]["overheat"] | false;
        S().printer.holdJobs = doc["printer"]["holdJobs"] | 0;
    }
    
    // Menu復元
    S().menu.clear();
    if (doc["menu"].is<JsonArray>()) {
        for (JsonVariantConst v : doc["menu"].as<JsonArrayConst>()) {
            MenuItem item;
            item.sku = v["sku"] | "";
            item.name = v["name"] | "";
            item.nameRomaji = v["nameRomaji"] | "";
            item.category = v["category"] | "";
            item.active = v["active"] | true;
            item.price_normal = v["price_normal"] | 0;
            item.price_presale = v["price_presale"] | 0;
            item.presale_discount_amount = v["presale_discount_amount"] | 0;
            item.price_single = v["price_single"] | 0;
            item.price_as_side = v["price_as_side"] | 0;
            S().menu.push_back(item);
        }
    }
    
    // Orders復元  
    S().orders.clear();
    if (doc["orders"].is<JsonArray>()) {
        for (JsonVariantConst v : doc["orders"].as<JsonArrayConst>()) {
            Order order;
            order.orderNo = v["orderNo"] | "";
            order.status = v["status"] | "";
            order.ts = v["ts"] | 0;
            order.printed = v["printed"] | false;
            order.cancelReason = v["cancelReason"] | "";
            // 追加: 呼び出し画面関連フラグを復元
            order.cooked = v["cooked"] | false;
            order.picked_up = v["picked_up"] | false;
            order.pickup_called = v["pickup_called"] | false;
            
            if (v["items"].is<JsonArray>()) {
                for (JsonVariantConst iv : v["items"].as<JsonArrayConst>()) {
                    LineItem item;
                    item.sku = iv["sku"] | "";
                    item.name = iv["name"] | "";
                    item.qty = iv["qty"] | 1;
                    item.unitPriceApplied = iv["unitPriceApplied"] | 0;
                    item.priceMode = iv["priceMode"] | "";
                    item.kind = iv["kind"] | "";
                    item.unitPrice = iv["unitPrice"] | 0;
                    item.discountName = iv["discountName"] | "";
                    item.discountValue = iv["discountValue"] | 0;
                    order.items.push_back(item);
                }
            }
            
            Serial.printf("  復元: 注文 %s: status=%s, cooked=%d, picked_up=%d, pickup_called=%d, items=%d件\n",
                          order.orderNo.c_str(), order.status.c_str(), 
                          order.cooked, order.picked_up, order.pickup_called, order.items.size());
            
            S().orders.push_back(order);
        }
    }
    
    Serial.printf("スナップショット読込完了: %s\n", filename.c_str());
    Serial.printf("復元されたデータ: 注文数=%d件, メニュー数=%d件\n", S().orders.size(), S().menu.size());
    
    // メニューが空の場合、初期メニューを投入
    if (S().menu.empty()) {
        Serial.println("スナップショットにメニューが含まれていないため、初期メニューを投入");
        ensureInitialMenu();
    } else {
        Serial.printf("スナップショットからメニュー復元: %d件\n", S().menu.size());
    }
    
    return true;
}

bool walAppend(const String& line) {
    // ディレクトリ作成（存在しない場合）
    if (!LittleFS.exists("/kds")) {
        if (!LittleFS.mkdir("/kds")) {
            Serial.println("ディレクトリ作成失敗: /kds");
            return false;
        }
        Serial.println("ディレクトリ作成完了: /kds");
    }
    
    File file = LittleFS.open("/kds/wal.log", "a");
    if (!file) {
        Serial.println("WAL追記失敗");
        return false;
    }
    
    // タイムスタンプ付きで記録
    uint32_t ts = time(nullptr);
    file.printf("%u,%s\n", ts, line.c_str());
    file.flush();
    file.close();
    
    Serial.printf("WAL追記: %u,%s\n", ts, line.c_str());
    return true;
}

bool recoverToLatest(String &outLastTs) {
    Serial.println("=== recoverToLatest開始 ===");
    
    // まずスナップショット読込
    Serial.println("スナップショット読み込み中...");
    if (!snapshotLoad()) {
        outLastTs = "snapshot load failed";
        Serial.println("エラー: スナップショット読み込み失敗");
        return false;
    }
    
    Serial.printf("スナップショット読み込み成功: 注文数=%d, メニュー数=%d\n", 
                  S().orders.size(), S().menu.size());
    
    // WAL適用
    File walFile = LittleFS.open("/kds/wal.log", "r");
    if (!walFile) {
        outLastTs = "no WAL file";
        return true; // WALがないのは正常
    }
    
    String lastTimestamp = "";
    while (walFile.available()) {
        String line = walFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        
        int firstComma = line.indexOf(',');
        if (firstComma < 0) continue;
        
        String tsStr = line.substring(0, firstComma);
        String operation = line.substring(firstComma + 1);
        
        // タイムスタンプ記録
        lastTimestamp = tsStr;
        
        // 操作解釈（簡易実装）
        Serial.printf("WAL適用: %s - %s\n", tsStr.c_str(), operation.c_str());
        
        // TODO: 実際の操作適用（ORDER_CREATE, MAIN_UPSERT等）
        // 今回は基本ログ出力のみ
    }
    
    walFile.close();
    
    // 人間readable形式に変換
    if (lastTimestamp.length() > 0) {
        uint32_t ts = lastTimestamp.toInt();
        struct tm* timeinfo = localtime((time_t*)&ts);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
        outLastTs = String(buffer);
    } else {
        outLastTs = "no operations";
    }
    
    Serial.printf("復元完了: 最終タイムスタンプ %s\n", outLastTs.c_str());
    return true;
}

// メニューを強制的に再作成（デバッグ用）
void forceCreateInitialMenu() {
    Serial.println("=== 初期メニュー強制作成開始 ===");
    int previousCount = S().menu.size();
    Serial.printf("作成前メニュー数: %d\n", previousCount);
    
    S().menu.clear();
    Serial.println("メニューをクリアしました");
    
    createInitialMenuItems();
    
    Serial.printf("初期メニュー強制作成完了: %d件\n", S().menu.size());
    
    // 作成されたメニューを確認
    for (int i = 0; i < S().menu.size(); i++) {
        Serial.printf("作成[%d]: %s (%s) - %s\n", 
                     i, S().menu[i].sku.c_str(), S().menu[i].name.c_str(), S().menu[i].category.c_str());
    }
}

// 実際のメニューアイテム作成ロジック
void createInitialMenuItems() {
    // デフォルトちんちろ設定
    S().settings.chinchiro.enabled = true;
    S().settings.chinchiro.multipliers = {0, 0.5, 1, 2, 3};
    S().settings.chinchiro.rounding = "round";
    
    // セッションID生成
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d-%p", &timeinfo);
        S().session.sessionId = String(buffer);
    } else {
        S().session.sessionId = "2025-09-25-AM";
    }
    S().session.startedAt = time(nullptr);
    
    // メインメニュー（A/B/Cバーガー）
    MenuItem mainA;
    mainA.sku = "main_0001";
    mainA.name = "Aバーガー";
    mainA.nameRomaji = "A Burger";
    mainA.category = "MAIN";
    mainA.active = true;
    mainA.price_normal = 500;
    mainA.price_presale = 0; // 計算で求める
    mainA.presale_discount_amount = -100;
    S().menu.push_back(mainA);
    
    MenuItem mainB;
    mainB.sku = "main_0002";
    mainB.name = "Bバーガー";
    mainB.nameRomaji = "B Burger";
    mainB.category = "MAIN";
    mainB.active = true;
    mainB.price_normal = 600;
    mainB.price_presale = 0;
    mainB.presale_discount_amount = -100;
    S().menu.push_back(mainB);
    
    MenuItem mainC;
    mainC.sku = "main_0003";
    mainC.name = "Cバーガー";
    mainC.nameRomaji = "C Burger";
    mainC.category = "MAIN";
    mainC.active = true;
    mainC.price_normal = 700;
    mainC.price_presale = 0;
    mainC.presale_discount_amount = -100;
    S().menu.push_back(mainC);
    
    // サイドメニュー（ドリンクA-D）
    const char* drinks[] = {"ドリンクA", "ドリンクB", "ドリンクC", "ドリンクD"};
    const char* drinksRomaji[] = {"Drink A", "Drink B", "Drink C", "Drink D"};
    for (int i = 0; i < 4; i++) {
        MenuItem drink;
        drink.sku = "side_000" + String(i + 1);
        drink.name = drinks[i];
        drink.nameRomaji = drinksRomaji[i];
        drink.category = "SIDE";
        drink.active = true;
        drink.price_single = 200;
        drink.price_as_side = 100;
        S().menu.push_back(drink);
    }
    
    // ポテトS
    MenuItem potato;
    potato.sku = "side_0005";
    potato.name = "ポテトS";
    potato.nameRomaji = "French Fries S";
    potato.category = "SIDE";
    potato.active = true;
    potato.price_single = 300;
    potato.price_as_side = 150;
    S().menu.push_back(potato);
    
    Serial.printf("初期メニュー投入完了: %d件\n", S().menu.size());
    
    // デバッグ: 投入されたメニューを出力
    Serial.println("=== 初期メニュー詳細 ===");
    for (int i = 0; i < S().menu.size(); i++) {
        const auto& item = S().menu[i];
        Serial.printf("メニュー%d: %s (%s) - SKU:%s\n", 
                     i+1, item.name.c_str(), item.category.c_str(), item.sku.c_str());
        if (item.category == "MAIN") {
            Serial.printf("  通常価格:%d円, 前売割引:%d円\n", 
                         item.price_normal, item.presale_discount_amount);
        } else {
            Serial.printf("  単品:%d円, セット:%d円\n", 
                         item.price_single, item.price_as_side);
        }
    }
}

void ensureInitialMenu() {
    if (!S().menu.empty()) {
        Serial.printf("初期メニュー確認: 既存メニュー数 %d\n", S().menu.size());
        for (int i = 0; i < S().menu.size(); i++) {
            Serial.printf("  [%d] %s (%s) - %s\n", 
                         i, S().menu[i].sku.c_str(), S().menu[i].name.c_str(), S().menu[i].category.c_str());
        }
        return; // 既にメニューがある
    }
    
    Serial.println("初期メニューを投入中...");
    createInitialMenuItems();
    
    // 初期スナップショット保存
    snapshotSave();
}