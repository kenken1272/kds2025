#include "printer_queue.h"
#include "printer_render.h"
#include <deque>

// 外部関数宣言（main.cppで定義）
extern String getCurrentDateTime();
extern bool isTimeValid();

static std::deque<OrderPrintJob> printQueue;
static std::deque<OrderPrintJob> holdQueue; // Hold for paper out

String formatOrderTicket(const Order& order) {
    String ticket = "";
    
    // Debug: Check order data
    Serial.printf("=== ATOM Printer Receipt Format ===\n");
    Serial.printf("Order No: %s\n", order.orderNo.c_str());
    Serial.printf("Item count: %d\n", order.items.size());
    Serial.printf("Order time: %lu\n", order.ts);
    
    // English printing for ATOM Printer: ASCII only, ESC/POS direct commands
    // Optimized for thermal printer stability
    
    // Header: Store name and order number (English)
    // 注意: String にバイナリ制御コード(0x00含む)を直接埋めない方針。
    // ここでは可視ASCIIのみを構築し、制御系(改行/カット)は PrinterRenderer 側APIで実行する。
    // （旧実装での "\x1B\x61\x01" などは削除）
    ticket += S().settings.store.nameRomaji + "\n";
    ticket += "========================\n";
    
    // Display order number in large size
    // (旧) 文字拡大 ESC/POS シーケンス禁止: Stringにバイナリを入れない
    // ここでの装飾は将来 PrinterRenderer に委譲する
    ticket += "Order No: " + order.orderNo + "\n";
    // (旧) 戻しコマンド削除
    
    // Display order time (current time - more reliable)
    String currentTime = isTimeValid() ? getCurrentDateTime() : "Time not synced";
    Serial.printf("Time info: ts=%lu, current=%s\n", order.ts, currentTime.c_str());
    
    ticket += "Date: " + currentTime + "\n";
    ticket += "------------------------\n";
    
    // Item details (English only)
    int total = 0;
    Serial.printf("Item details:\n");
    for (const auto& item : order.items) {
        if (item.kind == "ADJUST") {
            Serial.printf("Skip adjustment row: %s\n", item.name.c_str());
            continue; // Exclude adjustment rows
        }
        
        Serial.printf("- %s: qty=%d, price=%d, kind=%s\n", 
                     item.name.c_str(), item.qty, item.unitPriceApplied, item.kind.c_str());
        
        // Search for romaji name from menu
        String romajiName = item.name; // Default is original name
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
        
        // Item line (romaji name)
        String displayName = romajiName.length() > 0 ? romajiName : item.name;
        if (displayName.length() > 20) displayName = displayName.substring(0, 20);
        
        ticket += displayName + "\n";
        ticket += "  x" + String(qty) + " ";
        
        // Display price mode
        if (item.priceMode == "presale") {
            ticket += "(Pre) ";
        }
        
        ticket += String(unitPrice) + "yen\n";
        ticket += "  Subtotal: " + String(lineTotal) + "yen\n";
    }
    
    Serial.printf("Total amount: %d yen\n", total);
    
    // Total amount (English, bold)
    ticket += "------------------------\n";
    // (旧) Bold開始/終了削除
    ticket += "TOTAL: " + String(total) + " YEN";
    ticket += "\n";
    
    ticket += "========================\n";
    
    // Footer (English)
    // (旧) 中央寄せESC削除 – Web側表示のみ
    ticket += "Thank you!\n";
    ticket += S().settings.store.nameRomaji + "\n";
    // (旧) 左寄せ戻し削除
    
    // Margin and cut
    ticket += "\n"; // 単純改行のみ。フィード/カットは送信時に renderer が行う
    
    return ticket;
}

void enqueuePrint(const Order& order) {
    OrderPrintJob job;
    job.orderNo = order.orderNo;
    job.content = formatOrderTicket(order); // Keep legacy format too
    job.enqueuedAt = time(nullptr);
    job.retryCount = 0;
    
    if (S().printer.paperOut) {
        // Paper out: Move to HOLD queue
        holdQueue.push_back(job);
        S().printer.holdJobs = holdQueue.size();
        Serial.printf("Print HOLD: %s (paper out)\n", order.orderNo.c_str());
    } else {
        // Normal queue
        printQueue.push_back(job);
        Serial.printf("Print queue added: %s\n", order.orderNo.c_str());
    }
}

void tickPrintQueue() {
    if (printQueue.empty() || S().printer.paperOut) {
        return;
    }
    
    // Process first job
    OrderPrintJob& job = printQueue.front();
    
    // Get order data and print receipt
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
        
        // Check printer renderer status
        if (!g_printerRenderer.isReady()) {
            Serial.println("Error: Printer renderer not initialized");
            return;
        }
        
        // ATOM Printer initialization before order printing
        Serial.printf("[PRINT] ATOM Printer init before order: %s\n", job.orderNo.c_str());
        g_printerRenderer.printerInit();
        
        // Use English text printing system (faster and more reliable)
        if (g_printerRenderer.printReceiptEN(*orderPtr)) {
            Serial.printf("[PRINT] English print success: %s\n", job.orderNo.c_str());
            // Pop only on print success
            printQueue.pop_front();
        } else {
            Serial.printf("[PRINT] English print failed: %s\n", job.orderNo.c_str());
            job.retryCount++;
            if (job.retryCount < 3) {
                // Return to retry queue with delay
                OrderPrintJob retryJob = job;
                printQueue.pop_front();
                printQueue.push_back(retryJob);
                Serial.printf("ATOM Printer retry scheduled: %s (attempt %d/3)\n", job.orderNo.c_str(), job.retryCount);
                delay(1000); // 1秒待機してプリンターを安定化
            } else {
                Serial.printf("ATOM Printer job abandoned: %s (max retry reached)\n", job.orderNo.c_str());
                printQueue.pop_front();
            }
        }
    } else {
        // ★ When order data not arrived, retry (don't discard job)
        Serial.printf("Order data not arrived: %s → retry later\n", job.orderNo.c_str());
        
        // Debug: Display available order list
        Serial.printf("Available orders: %d\n", S().orders.size());
        for (int i = 0; i < S().orders.size(); i++) {
            Serial.printf("  [%d] %s (items: %d)\n", 
                         i, S().orders[i].orderNo.c_str(), S().orders[i].items.size());
        }
        
        // Plan A: Move first job to end for retry
        OrderPrintJob retryJob = job;
        printQueue.pop_front();
        printQueue.push_back(retryJob);
        return; // ★ This branch doesn't pop_front() except the initial pop
    }
}

void onPaperReplaced() {
    S().printer.paperOut = false;
    
    // Move HOLD queue to normal queue
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