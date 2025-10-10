#ifndef PRINTER_RENDER_H
#define PRINTER_RENDER_H

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include "store.h"

struct PrintOrderData {
  String orderNo;
  String storeName;
  std::vector<String> items;
  std::vector<String> itemsRomaji;
  std::vector<int> quantities;
  std::vector<int> prices;
  int    totalAmount = 0;
  String dateTime;
  String footerMessage;
};

class PrinterRenderer {
public:
  PrinterRenderer();
  ~PrinterRenderer();

  bool initialize(HardwareSerial* serial);
  void cleanup();

  void printerInit();
  void updateBaudRate(int baudRate);
  bool isReady() const;

  void sendFeedLines(int lines);
  void sendCutCommand();

  bool printEnglishTest();
  bool printJapaneseTest();
  bool printHelloWorldTest();

  bool printReceiptEN(const PrintOrderData& od);
  bool printReceiptEN(const Order& order);

  bool printReceiptJP(const PrintOrderData& od);
  bool printReceiptJP(const Order& order);

  bool printSelfCheck();
  bool printSelfCheckEscStar();

  bool printQRCode(const String& content);

private:
  static constexpr int DOT_WIDTH     = 384;
  static constexpr int RASTER_HEIGHT = 24;
  static constexpr int LINE_SPACING  = 24;

  static constexpr int PRN_RX = 33; 
  static constexpr int PRN_TX = 23; 

  HardwareSerial* printerSerial_ = nullptr;
  bool ready_ = false;
  int  baud_  = 115200;

  static String toASCII(const String& s);

  std::vector<uint8_t> buildMonoBand(M5Canvas& sp, int startY, int height) const;

  void sendRasterBand(const uint8_t* data, int bytesPerRow, int height);

  bool sendSpriteAsRaster(M5Canvas& sp);

  int drawStoreName(M5Canvas& sp, const String& name, int y);
  int drawOrderNumber(M5Canvas& sp, const String& orderNo, int y);
  int drawSeparator(M5Canvas& sp, int y);
  int drawItemsJP(M5Canvas& sp, const PrintOrderData& od, int y);
  int drawTotal(M5Canvas& sp, int total, int y);
  int drawDateTime(M5Canvas& sp, const String& dt, int y);
  int drawFooter(M5Canvas& sp, const String& footer, int y);

  void writeASCII(const String& s);
  void writeLineASCII(const String& s);
};

extern PrinterRenderer g_printerRenderer;

#endif