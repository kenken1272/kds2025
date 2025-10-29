#include "printer_queue.h"
#include "printer_render.h"
#include <deque>

extern String getCurrentDateTime();
extern bool isTimeValid();

nstatic std::deque<OrderPrintJob> printQueue;
static std::deque<OrderPrintJob> holdQueue;

String formatOrderTicket(const Order& order) {
    String ticket;
    ticket.reserve(256);

    ticket += S().settings.store.nameRomaji + "\n";
    ticket += "========================\n";
    ticket += "Order No: " + order.orderNo + "\n";

    String currentTime = isTimeValid() ? getCurrentDateTime() : "Time not synced";
    ticket += "Date: " + currentTime + "\n";
    ticket += "------------------------\n";

    int total = 0;
    for (const auto& item : order.items) {
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
        if (displayName.length() > 20) {
            displayName = displayName.substring(0, 20);
        }

        ticket += displayName + "\n";
        ticket += "  x" + String(qty) + " ";

        if (item.priceMode == "presale") {
            ticket += "(Pre) ";
        }

        ticket += String(unitPrice) + "yen\n";
        ticket += "  Subtotal: " + String(lineTotal) + "yen\n";
    }

    ticket += "------------------------\n";
    ticket += "TOTAL: " + String(total) + " YEN\n";
    ticket += "========================\n";
    ticket += "Thank you!\n";
    ticket += S().settings.store.nameRomaji + "\n\n";

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
    } else {
        printQueue.push_back(job);
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

    if (!orderPtr) {
        Serial.printf("[E] print order missing: %s\n", job.orderNo.c_str());
        OrderPrintJob retryJob = job;
        printQueue.pop_front();
        printQueue.push_back(retryJob);
        return;
    }

    if (!g_printerRenderer.isReady()) {
        Serial.println("[E] printer not ready");
        return;
    }

    g_printerRenderer.printerInit();

    if (g_printerRenderer.printReceiptEN(*orderPtr)) {
        Serial.printf("[PRINT] success: %s\n", job.orderNo.c_str());
        printQueue.pop_front();
    } else {
        Serial.printf("[E] print failed: %s\n", job.orderNo.c_str());
        job.retryCount++;
        if (job.retryCount < 3) {
            OrderPrintJob retryJob = job;
            printQueue.pop_front();
            printQueue.push_back(retryJob);
            delay(1000);
        } else {
            printQueue.pop_front();
        }
    }
}

void onPaperReplaced() {
    S().printer.paperOut = false;

    while (!holdQueue.empty()) {
        printQueue.push_back(holdQueue.front());
        holdQueue.pop_front();
    }

    S().printer.holdJobs = 0;
}

int getPendingPrintJobs() {
    return printQueue.size() + holdQueue.size();
}
