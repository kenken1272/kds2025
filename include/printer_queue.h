#pragma once
#include "store.h"
#include <deque>

struct OrderPrintJob {
    String orderNo;
    String content;
    uint32_t enqueuedAt{0};
    int retryCount{0};
};

/**
 * 印刷キューに注文を追加
 * @param order 印刷する注文
 */
void enqueuePrint(const Order& order);

/**
 * 印刷キューを処理（main loopから呼び出し）
 */
void tickPrintQueue();

/**
 * 用紙交換後の処理（HOLD分を再送）
 */
void onPaperReplaced();

/**
 * 印刷待ちジョブ数を取得
 * @return キュー内のジョブ数
 */
int getPendingPrintJobs();