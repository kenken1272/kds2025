#include <ESPAsyncWebServer.h>
#include "csv_export.h"
#include "store.h"

static inline void writeBOM(AsyncResponseStream *res) {
  static const uint8_t bom[3] = {0xEF,0xBB,0xBF};
  res->write(bom, 3);
}

static void writeOrderCsvRows(AsyncResponseStream* res, const Order& order, const String& sessionId) {
  int ln = 0;
  for (const auto& li : order.items) {
    ++ln;
    int lineTotal = li.unitPriceApplied * li.qty - li.discountValue;
    res->printf(
      "%u,%s,%s,%d,%s,%s,%d,%d,%s,%s,%d,%s\r\n",
      (unsigned)order.ts,
      sessionId.c_str(),
      order.orderNo.c_str(),
      ln,
      li.sku.c_str(),
      li.name.c_str(),
      li.qty,
      li.unitPriceApplied,
      li.priceMode.c_str(),
      li.kind.c_str(),
      lineTotal,
      order.status.c_str()
    );
  }
}

struct CsvArchiveContext {
  AsyncResponseStream* res{nullptr};
  String sessionId;
  CsvArchiveContext(AsyncResponseStream* stream, const String& session)
    : res(stream), sessionId(session) {}
};

static bool csvArchiveVisitor(const Order& order, const String& sessionId, uint32_t, void* ctx) {
  auto* context = static_cast<CsvArchiveContext*>(ctx);
  if (!context || !context->res) {
    return false;
  }
  writeOrderCsvRows(context->res, order, sessionId);
  return true;
}

void sendCsvStream(AsyncWebServerRequest *req){
  String fname = "attachment; filename=\"sales_" + S().session.sessionId + ".csv\"";
  AsyncResponseStream *res = req->beginResponseStream("text/csv");
  res->addHeader("Content-Disposition", fname);

  writeBOM(res);
  res->print("ts,sessionId,orderNo,lineNo,sku,name,qty,unitPriceApplied,priceMode,kind,lineTotal,status\r\n");

  for (const auto& o : S().orders) {
    writeOrderCsvRows(res, o, S().session.sessionId);
  }

  CsvArchiveContext ctx(res, S().session.sessionId);
  archiveForEach(S().session.sessionId, csvArchiveVisitor, &ctx);
  req->send(res);
}