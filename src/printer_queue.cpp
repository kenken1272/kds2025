#include "printer_queue.h"
#include "printer_render.h"
#include <deque>

extern String getCurrentDateTime();
extern bool isTimeValid();

static std::deque<OrderPrintJob> printQueue;
static std::deque<OrderPrintJob> holdQueue;

String formatOrderTicket(const Order& order) {
    String ticket = "";
    
    Serial.printf("=== ATOM Printer Receipt Format ===\n");
    Serial.printf("Order No: %s\n", order.orderNo.c_str());
    Serial.printf("Item count: %d\n", order.items.size());
    Serial.printf("Order time: %lu\n", order.ts);
    
    ticket += S().settings.store.nameRomaji + "\n";
    ticket += "========================\n";
    
    ticket += "Order No: " + order.orderNo + "\n";
    
    String currentTime = isTimeValid() ? getCurrentDateTime() : "Time not synced";
    Serial.printf("Time info: ts=%lu, current=%s\n", order.ts, currentTime.c_str());
    
    ticket += "Date: " + currentTime + "\n";
    ticket += "------------------------\n";
    
    int total = 0;
    Serial.printf("Item details:\n");
    for (const auto& item : order.items) {
        Serial.printf("- %s: qty=%d, price=%d, kind=%s\n", 
                     item.name.c_str(), item.qty, item.unitPriceApplied, item.kind.c_str());
        
        String romajiName = item.name;
        for (const auto& menuItem : S().menu) {
            if (menuItem.sku == item.sku || menuItem.name == item.name) {
                romajiName = menuItem.nameRomaji;
                break;
            }
        }
        
        int unitPrice = item.unitPriceApplied > 0 ? item.unitPriceApplied : item.unitPrice;
        int qty = item.qty > 0 ? item.qty : 1;
        int discount = item.discountValue > 0 ? item.discountValue : 0;
        int lineTotal = (unitPrice * qty) - discount;
        total += lineTotal;
        
        String displayName = romajiName.length() > 0 ? romajiName : item.name;
        if (displayName.length() > 20) displayName = displayName.substring(0, 20);
        
        ticket += displayName + "\n";
        ticket += "  x" + String(qty) + " ";
        
        if (item.priceMode == "presale") {
            ticket += "(Pre) ";
        }
        
        ticket += String(unitPrice) + "yen\n";
        ticket += "  Subtotal: " + String(lineTotal) + "yen\n";
    }
    
    Serial.printf("Total amount: %d yen\n", total);
    
    ticket += "------------------------\n";
    ticket += "TOTAL: " + String(total) + " YEN";
    ticket += "\n";
    
    ticket += "========================\n";
    
    ticket += "Thank you!\n";
    ticket += S().settings.store.nameRomaji + "\n";
    
    ticket += "\n";
    
    return ticket;
}

void enqueuePrint(const Order& order) {
    OrderPrintJob job;
    job.orderNo = order.orderNo;
    job.content = formatOrderTicket(order);
    job.enqueuedAt = time(nullptr);
    job.retryCount = 0;
    
    if (S().printer.paperOut) {
        holdQueue.push_back(job);
        S().printer.holdJobs = holdQueue.size();
        Serial.printf("Print HOLD: %s (paper out)\n", order.orderNo.c_str());
    } else {
        printQueue.push_back(job);
        Serial.printf("Print queue added: %s\n", order.orderNo.c_str());
    }
}

void tickPrintQueue() {
    if (printQueue.empty() || S().printer.paperOut) {
        return;
    }
    
    OrderPrintJob& job = printQueue.front();
    
    Order* orderPtr = nullptr;
    for (auto& order : S().orders) {
        if (order.orderNo == job.orderNo) {
            orderPtr = &order;
            break;
        }
    }
    
    if (orderPtr) {
        Serial.printf("Print start: Order data found %s (items: %d)\n", 
                     job.orderNo.c_str(), orderPtr->items.size());
        
        if (!g_printerRenderer.isReady()) {
            Serial.println("Error: Printer renderer not initialized");
            return;
        }
        
        Serial.printf("[PRINT] ATOM Printer init before order: %s\n", job.orderNo.c_str());
        g_printerRenderer.printerInit();
        
        if (g_printerRenderer.printReceiptEN(*orderPtr)) {
            Serial.printf("[PRINT] English print success: %s\n", job.orderNo.c_str());
            printQueue.pop_front();
        } else {
            Serial.printf("[PRINT] English print failed: %s\n", job.orderNo.c_str());
            job.retryCount++;
            if (job.retryCount < 3) {
                OrderPrintJob retryJob = job;
                printQueue.pop_front();
                printQueue.push_back(retryJob);
                Serial.printf("ATOM Printer retry scheduled: %s (attempt %d/3)\n", job.orderNo.c_str(), job.retryCount);
                delay(1000);
            } else {
                Serial.printf("ATOM Printer job abandoned: %s (max retry reached)\n", job.orderNo.c_str());
                printQueue.pop_front();
            }
        }
    } else {
        Serial.printf("Order data not arrived: %s → retry later\n", job.orderNo.c_str());
        
        Serial.printf("Available orders: %d\n", S().orders.size());
        for (int i = 0; i < S().orders.size(); i++) {
            Serial.printf("  [%d] %s (items: %d)\n", 
                         i, S().orders[i].orderNo.c_str(), S().orders[i].items.size());
        }
        
        OrderPrintJob retryJob = job;
        printQueue.pop_front();
        printQueue.push_back(retryJob);
        return;
    }
}

void onPaperReplaced() {
    S().printer.paperOut = false;
    
    while (!holdQueue.empty()) {
        printQueue.push_back(holdQueue.front());
        holdQueue.pop_front();
    }
    
    S().printer.holdJobs = 0;
    
    Serial.printf("Paper replacement completed: %d jobs moved to resend queue\n", printQueue.size());
}

int getPendingPrintJobs() {
    return printQueue.size() + holdQueue.size();
}