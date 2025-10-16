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

  JsonDocument doc;
  doc.set(req);

  auto findMenu = [&](const String& sku)->const MenuItem*{
    for (const auto& item : S().menu) {
      if (item.sku == sku) {
        return &item;
      }
    }
    return nullptr;
  };

  if (!doc["lines"].is<JsonArray>()) {
    Serial.println("[E] order lines missing");
    return o;
  }

  JsonArray lines = doc["lines"].as<JsonArray>();
  for (JsonVariant v : lines){
    const char* type = v["type"] | "";
    int qty = v["qty"] | 1;

    if (strcmp(type, "SET") == 0){
      String mainSku = String(v["mainSku"] | "");
      String pm = String(v["priceMode"] | "normal");

      auto main = findMenu(mainSku);
      if(!main) {
        Serial.printf("[E] menu missing: %s\n", mainSku.c_str());
        continue;
      }
      if(main->category != "MAIN") {
        Serial.printf("[E] menu wrong category: %s\n", mainSku.c_str());
        continue;
      }

      LineItem lm;
      lm.sku = mainSku;
      lm.name = main->name;
      lm.qty = qty;
      lm.kind = "MAIN";
      lm.priceMode = (pm == "presale") ? "presale" : "normal";
      int base = (lm.priceMode=="presale" && main->price_presale>0)
        ? main->price_presale
        : (main->price_normal + (lm.priceMode=="presale" ? main->presale_discount_amount : 0));
      lm.unitPriceApplied = lm.unitPrice = base;
      o.items.push_back(lm);

      int setSubtotal = base;
      if (v["sideSkus"].is<JsonArray>()) {
        JsonArray sides = v["sideSkus"].as<JsonArray>();
        for (JsonVariant sv : sides){
          String sideSku = String(sv | "");
          auto side = findMenu(sideSku);
          if(!side) {
            Serial.printf("[E] side menu missing: %s\n", sideSku.c_str());
            continue;
          }
          if(side->category != "SIDE") {
            Serial.printf("[E] side category mismatch: %s\n", sideSku.c_str());
            continue;
          }
          LineItem ls;
          ls.sku = sideSku;
          ls.name = side->name;
          ls.qty = qty;
          ls.kind = "SIDE_AS_SET";
          ls.priceMode = "";
          ls.unitPriceApplied = ls.unitPrice = side->price_as_side;
          o.items.push_back(ls);
          setSubtotal += side->price_as_side;
        }
      }

      JsonVariant multiplierVar = v["chinchoiroMultiplier"];
      if (S().settings.chinchiro.enabled && !multiplierVar.isNull()) {
        float multiplier = multiplierVar.as<float>();
        String result = String(v["chinchoiroResult"] | "");
        if (result.isEmpty()) {
          result = String(multiplier, 2) + "x";
        }

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
          }
        }
      }
    }
    else if (strcmp(type, "MAIN_SINGLE") == 0){
      String mainSku = String(v["mainSku"] | "");
      String pm = String(v["priceMode"] | "normal");

      auto main = findMenu(mainSku);
      if(!main) {
        Serial.printf("[E] menu missing: %s\n", mainSku.c_str());
        continue;
      }
      if(main->category != "MAIN") {
        Serial.printf("[E] menu wrong category: %s\n", mainSku.c_str());
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
    }
    else if (strcmp(type, "SIDE_SINGLE") == 0){
      String sideSku = String(v["sideSku"] | "");
      auto side = findMenu(sideSku);
      if(!side) {
        Serial.printf("[E] side menu missing: %s\n", sideSku.c_str());
        continue;
      }
      if (side->category != "SIDE") {
        Serial.printf("[E] side category mismatch: %s\n", sideSku.c_str());
        continue;
      }
      LineItem l;
      l.sku = sideSku;
      l.name = side->name;
      l.qty = qty;
      l.kind = "SIDE_SINGLE";
      l.priceMode = "";
      l.unitPriceApplied = l.unitPrice = side->price_single;
      o.items.push_back(l);
    }
  }

  if (o.items.empty()) {
    Serial.println("[E] order items empty");
    return o;
  }

  o.orderNo = allocateOrderNo();
  o.ts = time(nullptr);
  o.status = "COOKING";
  o.printed = false;

  return o;
}