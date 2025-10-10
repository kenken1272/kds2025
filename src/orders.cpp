#include "orders.h"
#include "store.h"
#include <ArduinoJson.h>
#include <cmath>

int calculateChinchoiroAdjustment(int setSubtotal, float multiplier, const String& rounding) {
    float rawAdjustment = setSubtotal * (multiplier - 1.0f);
    
    int adjustment = 0;
    if (rounding == "floor") {
        adjustment = (int)floor(rawAdjustment);
    } else if (rounding == "ceil") {
        adjustment = (int)ceil(rawAdjustment);
    } else { // "round"
        adjustment = (int)round(rawAdjustment);
    }
    
    return adjustment;
}

Order buildOrderFromClientJson(const JsonDocument& req){
  Order o;
  // ★ 採番は明細構築成功後に移動
  
  // ArduinoJson v7対応: const参照の問題を回避するため、コピーを作成
  JsonDocument doc;
  doc.set(req);

  auto findMenu = [&](const String& sku)->const MenuItem*{
    Serial.printf("メニュー検索: %s\n", sku.c_str());
    Serial.printf("利用可能メニュー数: %d\n", S().menu.size());
    for (int i = 0; i < S().menu.size(); i++) {
      Serial.printf("  [%d] SKU: %s, Name: %s, Category: %s\n", 
                   i, S().menu[i].sku.c_str(), S().menu[i].name.c_str(), S().menu[i].category.c_str());
      if (S().menu[i].sku == sku) {
        Serial.printf("メニュー見つかった: %s\n", sku.c_str());
        return &S().menu[i];
      }
    }
    Serial.printf("メニューが見つからない: %s\n", sku.c_str());
    return nullptr;
  };

  // v7対応：安全な型チェックと取得
  Serial.println("buildOrderFromClientJson開始");
  
  // デバッグ: 受信したJSONの内容を確認
  String debugJson;
  serializeJson(doc, debugJson);
  Serial.printf("buildOrderFromClientJson内で受信したJSON: %s\n", debugJson.c_str());
  
  // lines要素の存在確認（コピーしたdocを使用）
  Serial.printf("doc[\"lines\"]の型チェック:\n");
  Serial.printf("  doc.containsKey(\"lines\"): %s\n", doc["lines"].is<JsonArray>() ? "true" : "false");
  Serial.printf("  doc[\"lines\"].is<JsonArray>(): %s\n", doc["lines"].is<JsonArray>() ? "true" : "false");
  
  if (doc["lines"].is<JsonArray>()) {
    Serial.println("lines は配列です");
    JsonArray lines = doc["lines"].as<JsonArray>();
    Serial.printf("lines配列処理開始 - サイズ: %d\n", lines.size());
    int lineIndex = 0;
    for (JsonVariant v : lines){
      const char* type = v["type"] | "";
      int qty = v["qty"] | 1;
      Serial.printf("処理中line[%d]: type=%s, qty=%d\n", lineIndex++, type, qty);

      if (strcmp(type, "SET") == 0){
        const char* mainSkuCStr = v["mainSku"] | "";
        const char* pmCStr = v["priceMode"] | "normal";
        String mainSku = String(mainSkuCStr);
        String pm = String(pmCStr);
        
        Serial.printf("SET処理開始 - mainSku: %s, priceMode: %s, qty: %d\n", 
                     mainSku.c_str(), pm.c_str(), qty);
        
        auto main = findMenu(mainSku);
        if(!main) {
          Serial.printf("エラー: メニューが見つからない: %s\n", mainSku.c_str());
          continue;
        }
        if(main->category != "MAIN") {
          Serial.printf("エラー: カテゴリがMAINでない: %s (category: %s)\n", 
                       mainSku.c_str(), main->category.c_str());
          continue;
        }

        // MAIN 行
        LineItem lm;
        lm.sku = mainSku; lm.name = main->name; lm.qty = qty; lm.kind = "MAIN";
        lm.priceMode = (pm == "presale") ? "presale" : "normal";
        int base = (lm.priceMode=="presale" && main->price_presale>0)
          ? main->price_presale
          : (main->price_normal + (lm.priceMode=="presale" ? main->presale_discount_amount : 0));
        lm.unitPriceApplied = lm.unitPrice = base;
        o.items.push_back(lm);
        Serial.printf("MAIN追加: %s x%d (%d円) [%s]\n", 
                     lm.name.c_str(), lm.qty, lm.unitPriceApplied, lm.kind.c_str());

        // sideSkus
        int setSubtotal = base; // MAIN価格を初期値として設定
        if (v["sideSkus"].is<JsonArray>()) {
          JsonArray sides = v["sideSkus"].as<JsonArray>();
          Serial.printf("sideSkus処理開始 - サイズ: %d\n", sides.size());
          for (JsonVariant sv : sides){
            const char* sideSkuCStr = sv | "";
            String sideSku = String(sideSkuCStr);
            Serial.printf("sideSku処理: %s\n", sideSku.c_str());
            auto side = findMenu(sideSku);
            if(!side) {
              Serial.printf("エラー: sideメニューが見つからない: %s\n", sideSku.c_str());
              continue;
            }
            if(side->category != "SIDE") {
              Serial.printf("エラー: sideカテゴリがSIDEでない: %s (category: %s)\n", 
                           sideSku.c_str(), side->category.c_str());
              continue;
            }
            LineItem ls;
            ls.sku = sideSku; ls.name = side->name; ls.qty = qty; ls.kind = "SIDE_AS_SET"; ls.priceMode = "";
            ls.unitPriceApplied = ls.unitPrice = side->price_as_side;
            o.items.push_back(ls);
            setSubtotal += side->price_as_side; // SETの小計に加算
            Serial.printf("SIDE追加: %s x%d (%d円) [%s]\n", 
                         ls.name.c_str(), ls.qty, ls.unitPriceApplied, ls.kind.c_str());
          }
        } else {
          Serial.println("sideSkus配列が存在しないかNULL");
        }
        
        // ちんちろ調整（SET商品のみ）
        if (S().settings.chinchiro.enabled && v["chinchoiroMultiplier"].is<float>()) {
          float multiplier = v["chinchoiroMultiplier"].as<float>();
          const char* resultCStr = v["chinchoiroResult"] | "";
          String result = String(resultCStr);
          
          if (multiplier != 1.0f) {
            int adjustment = calculateChinchoiroAdjustment(setSubtotal, multiplier, S().settings.chinchiro.rounding);
            
            if (adjustment != 0) {
              LineItem adj;
              adj.sku = "CHINCHIRO_ADJUST";
              adj.name = String("Chinchiro (") + result + ")";
              adj.qty = qty;
              adj.kind = "ADJUST";
              adj.priceMode = "";
              adj.unitPriceApplied = adj.unitPrice = adjustment;
              adj.discountValue = 0;
              o.items.push_back(adj);
              Serial.printf("ちんちろ調整追加: %s (%d円 × %d) [倍率: %.2f]\n", 
                           adj.name.c_str(), adjustment, qty, multiplier);
            }
          }
        }
      }
      else if (strcmp(type, "MAIN_SINGLE") == 0){
        // メイン商品単品
        const char* mainSkuCStr = v["mainSku"] | "";
        const char* pmCStr = v["priceMode"] | "normal";
        String mainSku = String(mainSkuCStr);
        String pm = String(pmCStr);
        
        Serial.printf("MAIN_SINGLE処理開始 - mainSku: %s, priceMode: %s, qty: %d\n", 
                     mainSku.c_str(), pm.c_str(), qty);
        
        auto main = findMenu(mainSku);
        if(!main) {
          Serial.printf("エラー: メニューが見つからない: %s\n", mainSku.c_str());
          continue;
        }
        if(main->category != "MAIN") {
          Serial.printf("エラー: カテゴリがMAINでない: %s (category: %s)\n", 
                       mainSku.c_str(), main->category.c_str());
          continue;
        }
        
        LineItem lm;
        lm.sku = mainSku; 
        lm.name = main->name; 
        lm.qty = qty; 
        lm.kind = "MAIN_SINGLE";
        lm.priceMode = (pm == "presale") ? "presale" : "normal";
        int base = (lm.priceMode=="presale" && main->price_presale>0)
          ? main->price_presale
          : (main->price_normal + (lm.priceMode=="presale" ? main->presale_discount_amount : 0));
        lm.unitPriceApplied = lm.unitPrice = base;
        o.items.push_back(lm);
        Serial.printf("MAIN_SINGLE追加: %s x%d (%d円) [%s]\n", 
                     lm.name.c_str(), lm.qty, lm.unitPriceApplied, lm.kind.c_str());
      }
      else if (strcmp(type, "SIDE_SINGLE") == 0){
        const char* sideSkuCStr = v["sideSku"] | "";
        String sideSku = String(sideSkuCStr);
        auto side = findMenu(sideSku);
        if(!side || side->category != "SIDE") continue;
        LineItem l;
        l.sku = sideSku; l.name = side->name; l.qty = qty; l.kind = "SIDE_SINGLE"; l.priceMode = "";
        l.unitPriceApplied = l.unitPrice = side->price_single;
        o.items.push_back(l);
      }
    }
  } else {
    Serial.println("エラー: lines配列が存在しないか配列でない！");
  }

  // ★ 明細が空の場合は採番せずに返す
  if (o.items.empty()) {
    Serial.println("エラー: 明細が空のため注文を生成しません");
    return o; // orderNo未設定のまま
  }

  // ★ 明細が確定したタイミングで初めて採番＆状態設定
  o.orderNo = allocateOrderNo();
  o.ts = time(nullptr);
  o.status = "COOKING";
  o.printed = false;

  // 最終結果デバッグ
  Serial.printf("buildOrderFromClientJson完了 - 注文番号: %s, 最終アイテム数: %d\n", 
               o.orderNo.c_str(), o.items.size());
  for (int i = 0; i < o.items.size(); i++) {
    const auto& item = o.items[i];
    Serial.printf("  最終アイテム[%d]: %s x%d (%d円) [%s]\n", 
                 i, item.name.c_str(), item.qty, item.unitPriceApplied, item.kind.c_str());
  }

  // TODO: ちんちろ倍率の調整行をここで付与（必要なら）
  return o;
}