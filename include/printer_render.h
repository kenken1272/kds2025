#ifndef PRINTER_RENDER_H
#define PRINTER_RENDER_H

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include "store.h"
// 注文データ
struct PrintOrderData {
  String orderNo;                    // 注文番号（例: "0055"）
  String storeName;                  // 店舗名（ローマ字）
  std::vector<String> items;         // 商品名（日本語）
  std::vector<String> itemsRomaji;   // 商品名（ローマ字）
  std::vector<int> quantities;       // 数量
  std::vector<int> prices;           // 単価（円）
  int    totalAmount = 0;            // 合計金額（円）
  String dateTime;                   // 日時 "YYYY/MM/DD HH:MM:SS"
  String footerMessage;              // フッタ（ローマ字）
};

class PrinterRenderer {
public:
  PrinterRenderer();
  ~PrinterRenderer();

  // 初期化・終了
  bool initialize(HardwareSerial* serial); // serialは begin せずに渡す（内部で begin する）
  void cleanup();

  // 就業前の完全初期化（ESC/POSセットアップ）
  void printerInit();                 // ESC @ → ESC R 0 → ESC t 0 → 行間設定
  void updateBaudRate(int baudRate);  // 19200/38400 など
  bool isReady() const;

  // 紙送り・（機種により）カット
  void sendFeedLines(int lines);
  void sendCutCommand();              // 多くの58mm機はカッタ非搭載（実機で無効でもOK）

  // テスト印字
  bool printEnglishTest();            // ASCIIテキスト直送
  bool printJapaneseTest();           // 日本語（ラスタ）
  bool printHelloWorldTest();         // 超ミニ疎通テスト（ASCII直接 + HEX DUMP）

  // レシート印字（英語）
  bool printReceiptEN(const PrintOrderData& od);
  bool printReceiptEN(const Order& order);

  // レシート印字（日本語ラスタ）
  bool printReceiptJP(const PrintOrderData& od);
  bool printReceiptJP(const Order& order);

  // 簡易自己診断（黒バー）
  bool printSelfCheck();
  bool printSelfCheckEscStar();       // ESC * で真っ黒バー

private:
  // --- ハード仕様 ---
  static constexpr int DOT_WIDTH     = 384; // 58mm 幅（ドット）
  static constexpr int RASTER_HEIGHT = 24;  // GS v 0 の高さ単位（24ドット推奨）
  static constexpr int LINE_SPACING  = 24;  // ESC 3 の行間（ドット）


  static constexpr int PRN_RX = 33; 
  static constexpr int PRN_TX = 23; 

  HardwareSerial* printerSerial_ = nullptr;
  bool ready_ = false;
  int  baud_  = 115200; // Unified default baud (M5 ATOM Printer spec) – do not change silently

  // ---- 内部ユーティリティ ----
  // ASCIIのみに整形（0x20–0x7E以外を'?'へ）
  static String toASCII(const String& s);

  // 1バンド分（幅=384, 高さ=h）をモノクロ1bppに詰める（MSB→LSB, 8dot = 1byte）
  // spriteの y=[startY, startY+height) を対象に、白背景/黒文字前提で 1=黒 として詰める
  std::vector<uint8_t> buildMonoBand(M5Canvas& sp, int startY, int height) const;

  // GS v 0 でラスタ送信
  void sendRasterBand(const uint8_t* data, int bytesPerRow, int height);

  // Sprite全体をラスタ送信（高さに応じて分割）
  bool sendSpriteAsRaster(M5Canvas& sp);

  // レイアウト描画（日本語用：M5Canvas）
  int drawStoreName(M5Canvas& sp, const String& name, int y);
  int drawOrderNumber(M5Canvas& sp, const String& orderNo, int y);
  int drawSeparator(M5Canvas& sp, int y);
  int drawItemsJP(M5Canvas& sp, const PrintOrderData& od, int y);
  int drawTotal(M5Canvas& sp, int total, int y);
  int drawDateTime(M5Canvas& sp, const String& dt, int y);
  int drawFooter(M5Canvas& sp, const String& footer, int y);

  // 英語レシート（テキスト直送用ヘルパ）
  void writeASCII(const String& s);
  void writeLineASCII(const String& s); // s + "\n"
};

extern PrinterRenderer g_printerRenderer;

#endif // PRINTER_RENDER_H