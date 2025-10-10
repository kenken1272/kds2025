#include <ESPAsyncWebServer.h>
#include "csv_export.h"
#include "store.h"

static inline void writeBOM(AsyncResponseStream *res) {
  static const uint8_t bom[3] = {0xEF,0xBB,0xBF};
  res->write(bom, 3);
}

void sendCsvStream(AsyncWebServerRequest *req){
  String fname = "attachment; filename=\"sales_" + S().session.sessionId + ".csv\"";
  AsyncResponseStream *res = req->beginResponseStream("text/csv");
  res->addHeader("Content-Disposition", fname);

  writeBOM(res);
  res->print("ts,sessionId,orderNo,lineNo,sku,name,qty,unitPriceApplied,priceMode,kind,lineTotal,status\r\n");

  for (auto &o : S().orders){
    int ln = 0;
    for (auto &li : o.items){
      ++ln;
      int lineTotal = li.unitPriceApplied * li.qty - li.discountValue;
      res->printf(
        "%u,%s,%s,%d,%s,%s,%d,%d,%s,%s,%d,%s\r\n",
        (unsigned)o.ts,
        S().session.sessionId.c_str(),
        o.orderNo.c_str(),
        ln,
        li.sku.c_str(),
        li.name.c_str(),
        li.qty,
        li.unitPriceApplied,
        li.priceMode.c_str(),
        li.kind.c_str(),
        lineTotal,
        o.status.c_str()
      );
    }
  }
  req->send(res);
}