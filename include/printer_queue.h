#pragma once
#include "store.h"
#include <deque>

struct OrderPrintJob {
    String orderNo;
    String content;
    uint32_t enqueuedAt{0};
    int retryCount{0};
};

void enqueuePrint(const Order& order);

void tickPrintQueue();

void onPaperReplaced();

int getPendingPrintJobs();