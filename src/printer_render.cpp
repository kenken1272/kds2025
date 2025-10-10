#include "printer_render.h"
#include <M5Unified.h>
#include <time.h>

// -----------------------------------------------------------------------------
// デバッグ用：HEXダンプ & 低レベル送信ヘルパ（Hello World疎通テスト用）
// -----------------------------------------------------------------------------
static void _hexDump(const uint8_t* p, size_t n, const char* tag) {
  if (!p || n==0) { Serial.printf("[PRINT][DUMP] %s len=0 (null)\n", tag?tag:"?"); return; }
  Serial.printf("[PRINT][DUMP] %s len=%u : ", tag?tag:"?", (unsigned)n);
  for (size_t i=0;i<n;i++) {
    Serial.printf("%02X ", p[i]);
    if ((i+1)%16==0) Serial.print(" ");
  }
  Serial.println();
}

static void _sendBytesDebug(HardwareSerial* ser, const uint8_t* p, size_t n, const char* tag){
  if (!ser) return; _hexDump(p,n,tag); size_t w = ser->write(p,n); ser->flush(); Serial.printf("[PRINT][SEND](debug) %s wrote %u/%u bytes\n", tag?tag:"?", (unsigned)w,(unsigned)n); delay(10); }

// 成功判定付き送信（本タスク追加）
static bool _sendBytesChecked(HardwareSerial* ser, const uint8_t* p, size_t n, const char* tag) {
  if (!ser || !p || !n) return false;
  size_t w = ser->write(p, n);
  ser->flush();
  Serial.printf("[PRINT][SEND] %s wrote %u/%u bytes\n", tag?tag:"?", (unsigned)w, (unsigned)n);
  delay(10);
  return (w == n);
}

static void _sendLineASCII(HardwareSerial* ser, const String& s){
  if (!ser) return; String t = s; for (int i=0;i<t.length();++i){ char c=t[i]; if (c<0x20||c>0x7E) t.setCharAt(i,'?'); }
  Serial.printf("[PRINT][ASCII] \"%s\" (len=%d)\n", t.c_str(), t.length());
  ser->write((const uint8_t*)t.c_str(), t.length());
  ser->write((const uint8_t*)"\n",1);
  ser->flush();
}

// ---- 外部関数（main.cpp で定義）----
extern String getCurrentDateTime();
extern bool   isTimeValid();

// ---- グローバル ----
PrinterRenderer g_printerRenderer;

// =============================================================================
// コンストラクタ / 共通
// =============================================================================
PrinterRenderer::PrinterRenderer() = default;
PrinterRenderer::~PrinterRenderer() { cleanup(); }

bool PrinterRenderer::initialize(HardwareSerial* serial) {
  printerSerial_ = serial;
  if (!printerSerial_) {
    Serial.println("[PRINT] Error: Invalid printer serial");
    return false;
  }
  // Default baud unified to 115200bps (ESP32 RX=23, TX=33)
  baud_  = 115200;
  printerSerial_->begin(baud_, SERIAL_8N1, PRN_RX, PRN_TX);
  ready_ = true;
  Serial.printf("[PRINT] PrinterRenderer initialized (115200bps, RX=%d, TX=%d)\n", PRN_RX, PRN_TX);
  return true;
}

void PrinterRenderer::cleanup() {
  ready_        = false;
  printerSerial_ = nullptr;
}

bool PrinterRenderer::isReady() const { return ready_ && printerSerial_ != nullptr; }

// =============================================================================
// 初期化 / コマンド
// =============================================================================
void PrinterRenderer::printerInit() {
  if (!isReady()) {
    Serial.println("[PRINT] Error: printer serial not ready");
    return;
  }

  Serial.printf("[PRINT] ATOM Printer init @ %dbps (ESP32 RX=%d, TX=%d)\n", baud_, PRN_RX, PRN_TX);
  // begin(baud,config,RX,TX) 順序注意
  printerSerial_->begin(baud_, SERIAL_8N1, PRN_RX, PRN_TX);

  delay(200);
  while (printerSerial_->available()) { printerSerial_->read(); }
  printerSerial_->flush();

  const uint8_t ESC_AT[]     = {0x1B, 0x40};               // リセット
  const uint8_t ESC_R_USA[]  = {0x1B, 0x52, 0x00};         // 国際文字=USA
  const uint8_t ESC_T_437[]  = {0x1B, 0x74, 0x00};         // コードページ=PC437
  const uint8_t ESC_3_LS[]   = {0x1B, 0x33, (uint8_t)LINE_SPACING}; // 行間

  _sendBytesChecked(printerSerial_, ESC_AT,    sizeof(ESC_AT),    "ESC @ (reset)");
  _sendBytesChecked(printerSerial_, ESC_R_USA, sizeof(ESC_R_USA), "ESC R 0 (USA)");
  _sendBytesChecked(printerSerial_, ESC_T_437, sizeof(ESC_T_437), "ESC t 0 (PC437)");
  _sendBytesChecked(printerSerial_, ESC_3_LS,  sizeof(ESC_3_LS),  "ESC 3 line spacing");

  Serial.println("[PRINT] Initialization completed");
}

void PrinterRenderer::updateBaudRate(int baudRate) {
  if (!printerSerial_) {
    Serial.println("[PRINT] Error: Serial not initialized");
    return;
  }
  baud_ = baudRate;
  Serial.printf("[PRINT] Update baud -> %d\n", baud_);
  printerSerial_->end();
  delay(80);
  printerSerial_->begin(baud_, SERIAL_8N1, PRN_RX, PRN_TX);
  delay(120);
  printerInit(); // コードページ再設定
}

void PrinterRenderer::sendFeedLines(int lines) {
  if (!isReady() || lines <= 0) return;
  while (lines > 0) {
    uint8_t chunk = (uint8_t)min(lines, 255);
    const uint8_t feed[] = {0x1B, 0x64, chunk}; // ESC d n
    _sendBytesChecked(printerSerial_, feed, sizeof(feed), "ESC d n");
    delay(80 * chunk); // 推奨：1行 ~80ms
    lines -= chunk;
  }
}

void PrinterRenderer::sendCutCommand() {
  if (!isReady()) return;
  sendFeedLines(4);
  const uint8_t cut[] = {0x1D, 0x56, 0x42, 0x00}; // GS V B m=0x00 full cut
  _sendBytesChecked(printerSerial_, cut, sizeof(cut), "GS V B 0");
  Serial.println("[PRINT] Cut command issued");
}

// =============================================================================
// ASCII 整形 & 送信
// =============================================================================
String PrinterRenderer::toASCII(const String& s) {
  String out; out.reserve(s.length());
  for (size_t i=0; i<s.length(); ++i) {
    char c = s[i];
    out += (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  return out;
}

void PrinterRenderer::writeASCII(const String& s) {
  if (!isReady()) return;
  String t = toASCII(s);
  printerSerial_->write((const uint8_t*)t.c_str(), t.length());
}

void PrinterRenderer::writeLineASCII(const String& s) {
  if (!isReady()) return;
  writeASCII(s);
  const char nl = '\n';
  printerSerial_->write((const uint8_t*)&nl, 1);
}

// =============================================================================
// ラスタ用：モノクロ変換（行方向に詰める：widthBytes * height）
// =============================================================================
std::vector<uint8_t> PrinterRenderer::buildMonoBand(M5Canvas& sp, int startY, int height) const {
  const int width      = DOT_WIDTH;
  const int widthBytes = (width + 7) / 8;
  std::vector<uint8_t> out(widthBytes * height, 0);

  // readPixelで安全に取る（速度より堅牢性優先）
  for (int y = 0; y < height; ++y) {
    int srcY = startY + y;
    for (int x = 0; x < width; ++x) {
      uint16_t p = sp.readPixel(x, srcY);
      // 白背景前提。明度しきい値で2値化（128未満=黒）
      uint8_t r = ((p >> 11) & 0x1F) * 8;
      uint8_t g = ((p >> 5)  & 0x3F) * 4;
      uint8_t b = ( p        & 0x1F) * 8;
      uint16_t lum = (uint16_t)(r * 299 + g * 587 + b * 114) / 1000;
      if (lum < 128) {
        int byteIndex = y * widthBytes + (x >> 3);
        int bitIndex  = 7 - (x & 7);
        out[byteIndex] |= (1 << bitIndex); // 1=黒
      }
    }
  }
  return out;
}

// =============================================================================
// ラスタ送信（GS v 0）
// =============================================================================
void PrinterRenderer::sendRasterBand(const uint8_t* data, int bytesPerRow, int height) {
  if (!isReady() || !data || height <= 0) return;

  // GS v 0 m=0 : 正立, 通常密度
  const uint8_t hdr[] = {0x1D, 0x76, 0x30, 0x00};
  printerSerial_->write(hdr, sizeof(hdr));

  // xL xH : 横バイト数, yL yH : 縦ドット数
  const uint8_t xL = (uint8_t)(bytesPerRow & 0xFF);
  const uint8_t xH = (uint8_t)((bytesPerRow >> 8) & 0xFF);
  const uint8_t yL = (uint8_t)(height & 0xFF);
  const uint8_t yH = (uint8_t)((height >> 8) & 0xFF);

  printerSerial_->write(xL); printerSerial_->write(xH);
  printerSerial_->write(yL); printerSerial_->write(yH);

  // データ本体（行方向にそのまま）
  const size_t total = (size_t)bytesPerRow * (size_t)height;
  printerSerial_->write(data, total);
  printerSerial_->flush();

  // 軽いウェイト
  delay(10 + height); // 高さに応じて少し待つ
}

bool PrinterRenderer::sendSpriteAsRaster(M5Canvas& sp) {
  if (!isReady()) return false;

  const int widthBytes = (DOT_WIDTH + 7) / 8;
  const int h          = sp.height();

  Serial.printf("[PRINT] Raster send %d x %d (bytes/row=%d)\n", DOT_WIDTH, h, widthBytes);

  for (int y = 0; y < h; y += RASTER_HEIGHT) {
    const int bandH = min(RASTER_HEIGHT, h - y);
    auto band = buildMonoBand(sp, y, bandH);
    sendRasterBand(band.data(), widthBytes, bandH);
    delay(15);
  }
  return true;
}


bool PrinterRenderer::printSelfCheck() {
  if (!isReady()) return false;

  printerInit();

  M5Canvas canvas(&M5.Display);
  canvas.setColorDepth(16);
  canvas.createSprite(DOT_WIDTH, 220);
  canvas.fillScreen(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  canvas.setFont(&fonts::Font4);
  int y = 8;

  canvas.drawString("=== SELF CHECK ===", 10, y); y += 26;
  canvas.fillRect(10, y, DOT_WIDTH-20, 12, TFT_BLACK); y += 22;

  // チェッカ
  for (int r=0; r<5; ++r) {
    for (int c=0; c<16; ++c) {
      if (((r^c)&1)==0) canvas.fillRect(10+c*22, y+r*10, 18, 8, TFT_BLACK);
    }
  }
  y += 56;
  canvas.drawString("Raster: GS v 0", 10, y); y += 20;
  canvas.drawString("Codepage: PC437 / USA", 10, y); y += 20;
  canvas.drawString("Done.", 10, y);

  bool ok = sendSpriteAsRaster(canvas);
  canvas.deleteSprite();

  if (ok) { sendFeedLines(3); sendCutCommand(); }
  Serial.printf("[PRINT] Self-check: %s\n", ok ? "OK" : "NG");
  return ok;
}

// =============================================================================
// レイアウト（日本語はCanvas上に描画）
// =============================================================================
int PrinterRenderer::drawStoreName(M5Canvas& sp, const String& name, int y) {
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font7);
  sp.setTextSize(2);
  sp.drawString(name, DOT_WIDTH/2, y);
  return y + 32 + 8;
}

int PrinterRenderer::drawOrderNumber(M5Canvas& sp, const String& orderNo, int y) {
  // 注文番号を特大表示（サイズ4、太字、中央寄せ）
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font7);
  sp.setTextSize(4); // サイズを3→4へアップ
  
  // 上下に余白を追加
  y += 10;
  sp.drawString("Order No.", DOT_WIDTH/2, y);
  y += 56; // フォントサイズに合わせて調整
  sp.drawString(orderNo, DOT_WIDTH/2, y);
  y += 56 + 10; // 下余白
  
  return y + 8;
}

int PrinterRenderer::drawSeparator(M5Canvas& sp, int y) {
  sp.drawFastHLine(10, y, DOT_WIDTH-20, TFT_BLACK);
  return y + 2 + 8;
}

int PrinterRenderer::drawItemsJP(M5Canvas& sp, const PrintOrderData& od, int y) {
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font6);
  sp.setTextSize(1);

  const size_t n = od.itemsRomaji.size();
  for (size_t i=0; i<n; ++i) {
    const String name = (i < od.itemsRomaji.size()) ? od.itemsRomaji[i] : String("-");
    const int    qty  = (i < od.quantities.size())  ? od.quantities[i]  : 1;
    const int    unit = (i < od.prices.size())      ? od.prices[i]      : 0;

    sp.setTextDatum(textdatum_t::top_left);
    sp.drawString(name, 10, y);
    String right = "x" + String(qty) + "  " + String(unit) + "yen";
    sp.setTextDatum(textdatum_t::top_right);
    sp.drawString(right, DOT_WIDTH-10, y);
    y += 20;
  }
  return y + 4;
}

int PrinterRenderer::drawTotal(M5Canvas& sp, int total, int y) {
  sp.setTextDatum(textdatum_t::top_right);
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font7);
  sp.setTextSize(1);
  sp.drawString("Total: " + String(total) + " yen", DOT_WIDTH-10, y);
  return y + 26 + 6;
}

int PrinterRenderer::drawDateTime(M5Canvas& sp, const String& dt, int y) {
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font6);
  sp.setTextSize(1);
  sp.drawString(dt, DOT_WIDTH/2, y);
  return y + 18 + 6;
}

int PrinterRenderer::drawFooter(M5Canvas& sp, const String& footer, int y) {
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(TFT_BLACK, TFT_WHITE);
  sp.setFont(&fonts::Font6);
  sp.setTextSize(1);
  sp.drawString(footer, DOT_WIDTH/2, y);
  return y + 18 + 16;
}

// =============================================================================
// 日本語レシート（Canvas→ラスタ）
// =============================================================================
// =============================================================================
// 日本語レシート（小分割スプライト方式）
// メモリ節約：各セクションを個別に生成→送信→破棄
// =============================================================================
bool PrinterRenderer::printReceiptJP(const PrintOrderData& od) {
  if (!isReady()) return false;
  printerInit();

  // 小スプライト送信＋破棄ユーティリティ（private メソッドへは this 経由で可）
  auto sendAndDelete = [&](M5Canvas& sp){ bool ok = sendSpriteAsRaster(sp); sp.deleteSprite(); return ok; };

  // 1) 見出し（店名＋注文番号）
  {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 96);
    sp.fillScreen(TFT_WHITE);
    int y = 6;
    y = drawStoreName(sp, od.storeName, y);
    y = drawOrderNumber(sp, od.orderNo, y);
  if (!sendAndDelete(sp)) return false;
  }

  // 2) セパレータ
    {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 24);
    sp.fillScreen(TFT_WHITE);
    int y = 6;
    y = drawSeparator(sp, y);
  if (!sendAndDelete(sp)) return false;
  }

  // 3) 商品行（1行ずつ）
  for (size_t i = 0; i < od.itemsRomaji.size(); ++i) {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 28);
    sp.fillScreen(TFT_WHITE);

    PrintOrderData tmp; // 一行だけのデータコンテナ
    if (i < od.items.size())       tmp.items.push_back(od.items[i]);
    if (i < od.itemsRomaji.size()) tmp.itemsRomaji.push_back(od.itemsRomaji[i]);
    if (i < od.quantities.size())  tmp.quantities.push_back(od.quantities[i]);
    if (i < od.prices.size())      tmp.prices.push_back(od.prices[i]);

    int y = 2;
    y = drawItemsJP(sp, tmp, y); // 1行分
    (void)y;
  if (!sendAndDelete(sp)) return false;
  }

  // 4) セパレータ
  {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 24);
    sp.fillScreen(TFT_WHITE);
    int y = 6;
    y = drawSeparator(sp, y);
  if (!sendAndDelete(sp)) return false;
  }

  // 5) 合計
  {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 36);
    sp.fillScreen(TFT_WHITE);
    int y = 6;
    y = drawTotal(sp, od.totalAmount, y);
  if (!sendAndDelete(sp)) return false;
  }

  // 6) 日時
  {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 28);
    sp.fillScreen(TFT_WHITE);
    int y = 4;
    y = drawDateTime(sp, od.dateTime, y);
  if (!sendAndDelete(sp)) return false;
  }

  // 7) フッタ
  {
    M5Canvas sp(&M5.Display);
    sp.setColorDepth(16);
    sp.createSprite(DOT_WIDTH, 28);
    sp.fillScreen(TFT_WHITE);
    int y = 4;
    y = drawFooter(sp, od.footerMessage, y);
  if (!sendAndDelete(sp)) return false;
  }

  sendFeedLines(3);
  sendCutCommand();
  return true;
}

bool PrinterRenderer::printReceiptJP(const Order& order) {
  // Order -> PrintOrderData 変換（romaji名を採用）
  PrintOrderData od;
  od.orderNo    = order.orderNo;
  od.storeName  = S().settings.store.nameRomaji;
  od.footerMessage = "Thank you!";

  // 日時
  od.dateTime = isTimeValid() ? getCurrentDateTime() : "Time not synced";

  int total = 0;
  for (const auto& it : order.items) {
    // 調整行（ちんちろ等）も印刷対象に含める
    String romaji = it.name;
    
    // ADJUST以外はメニューからローマ字名を引く
    if (it.kind != "ADJUST") {
      for (const auto& m : S().menu) {
        if (m.sku == it.sku || m.name == it.name) { romaji = m.nameRomaji; break; }
      }
    }
    
    int unit = (it.unitPriceApplied > 0) ? it.unitPriceApplied : it.unitPrice;
    int qty  = it.qty > 0 ? it.qty : 1;
    int sub  = unit * qty - (it.discountValue > 0 ? it.discountValue : 0);

    od.items.push_back(romaji);
    od.itemsRomaji.push_back(romaji);
    od.quantities.push_back(qty);
    od.prices.push_back(unit);
    total += sub;
  }
  od.totalAmount = total;
  return printReceiptJP(od);
}

// =============================================================================
// 英語テスト / 日本語テスト
// =============================================================================
bool PrinterRenderer::printEnglishTest() {
  if (!isReady()) return false;
  printerInit();
  bool any=false;
  auto sendStr=[&](const String& s){ String line=toASCII(s)+"\n"; bool ok=_sendBytesChecked(printerSerial_, (const uint8_t*)line.c_str(), line.length(), "LINE"); any = any || ok; };
  sendStr("==============================");
  sendStr(" Kyudai Cooking Club - KyuShoku");
  sendStr("==============================");
  sendStr("");
  sendStr("Order No. 55");
  sendStr("------------------------------");
  sendStr("Teriyaki Burger      x1  800");
  sendStr("Kyushoku Burger      x1  700");
  sendStr("------------------------------");
  sendStr("Total:                  1500");
  sendStr(isTimeValid() ? getCurrentDateTime() : "Time not synced");
  sendStr("Thank you!");
  sendFeedLines(4);
  sendCutCommand();
  return any; // 1行も成功していなければ false
}

bool PrinterRenderer::printJapaneseTest() {
  if (!isReady()) return false;
  printerInit();

  auto sendAndDelete = [&](M5Canvas& sp){ bool ok = sendSpriteAsRaster(sp); sp.deleteSprite(); return ok; };

  // 見出し
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 60); sp.fillScreen(TFT_WHITE);
    sp.setTextColor(TFT_BLACK, TFT_WHITE); sp.setTextDatum(textdatum_t::top_center); sp.setFont(&fonts::Font7); sp.setTextSize(1);
    sp.drawString("九大料理サークルきゅう食", DOT_WIDTH/2, 6);
  if (!sendAndDelete(sp)) return false;
  }
  // 注文番号
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 48); sp.fillScreen(TFT_WHITE);
    sp.setTextColor(TFT_BLACK, TFT_WHITE); sp.setTextDatum(textdatum_t::top_left); sp.setFont(&fonts::Font7); sp.setTextSize(2);
    sp.drawString("注文番号 55番", 10, 4);
  if (!sendAndDelete(sp)) return false;
  }
  // セパレータ
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 24); sp.fillScreen(TFT_WHITE);
    sp.drawFastHLine(10, 12, DOT_WIDTH-20, TFT_BLACK);
  if (!sendAndDelete(sp)) return false;
  }
  // 行描画ラムダ
  auto _line = [&](const String& left, const String& right) {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 28); sp.fillScreen(TFT_WHITE);
    sp.setFont(&fonts::Font6); sp.setTextSize(1); sp.setTextColor(TFT_BLACK, TFT_WHITE);
    sp.setTextDatum(textdatum_t::top_left); sp.drawString(left, 10, 6);
    sp.setTextDatum(textdatum_t::top_right); sp.drawString(right, DOT_WIDTH-10, 6);
    return sendAndDelete(sp);
  };
  if (!_line("照り焼きバーガー", "x1   800yen")) return false;
  if (!_line("きゅう食バーガー", "x1   700yen")) return false;
  // セパレータ
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 24); sp.fillScreen(TFT_WHITE);
    sp.drawFastHLine(10, 12, DOT_WIDTH-20, TFT_BLACK);
  if (!sendAndDelete(sp)) return false;
  }
  // 合計
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 32); sp.fillScreen(TFT_WHITE);
    sp.setFont(&fonts::Font7); sp.setTextSize(1); sp.setTextColor(TFT_BLACK, TFT_WHITE); sp.setTextDatum(textdatum_t::top_right);
    sp.drawString("合計 1500円", DOT_WIDTH-10, 6);
  if (!sendAndDelete(sp)) return false;
  }
  // 日時
  {
    M5Canvas sp(&M5.Display); sp.setColorDepth(16); sp.createSprite(DOT_WIDTH, 26); sp.fillScreen(TFT_WHITE);
    sp.setFont(&fonts::Font6); sp.setTextSize(1); sp.setTextColor(TFT_BLACK, TFT_WHITE); sp.setTextDatum(textdatum_t::top_center);
    sp.drawString(isTimeValid() ? getCurrentDateTime() : "時刻未同期", DOT_WIDTH/2, 4);
  if (!sendAndDelete(sp)) return false;
  }

  sendFeedLines(3);
  sendCutCommand();
  return true;
}

// =============================================================================
// ESC * 専用の黒バー自己診断（任意）
// =============================================================================
bool PrinterRenderer::printSelfCheckEscStar() {
  if (!isReady()) return false;

  printerInit();

  // 真っ黒バー 384x120 を ESC * で送る（参考用）
  M5Canvas bar(&M5.Display);
  bar.setColorDepth(16);
  bar.createSprite(DOT_WIDTH, 120);
  bar.fillScreen(TFT_BLACK);

  const int widthBytes = (DOT_WIDTH + 7) / 8;
  auto band = buildMonoBand(bar, 0, 120);

  // ESC * 24-dot（m=33）で縦方向に送るため、転置して送信
  // ここでは簡便のため 24ラインごとに3バイト×横ドットで送る
  for (int y0=0; y0<120; y0+=24) {
    int blockH = min(24, 120 - y0);
    const uint8_t hdr[] = {0x1B, 0x2A, 33, (uint8_t)(DOT_WIDTH & 0xFF), (uint8_t)(DOT_WIDTH >> 8)};
    printerSerial_->write(hdr, sizeof(hdr));

    for (int x=0; x<DOT_WIDTH; ++x) {
      uint8_t b[3] = {0,0,0};
      for (int bit=0; bit<blockH; ++bit) {
        int srcY = y0 + bit;
        int idx  = srcY * widthBytes + (x >> 3);
        uint8_t mask = (uint8_t)(0x80 >> (x & 7));
        bool on = (idx < (int)band.size()) && (band[idx] & mask);
        if (on) {
          if (bit < 8)       b[0] |= (uint8_t)(1 << (7 - bit));
          else if (bit <16 ) b[1] |= (uint8_t)(1 << (15 - bit));
          else               b[2] |= (uint8_t)(1 << (23 - bit));
        }
      }
      printerSerial_->write(b, 3);
    }
    const uint8_t nl = 0x0A;
    printerSerial_->write(&nl, 1);
    delay(5);
  }

  sendFeedLines(3);
  sendCutCommand();
  bar.deleteSprite();
  return true;
}


// =============================================================================
bool PrinterRenderer::printReceiptEN(const PrintOrderData& od) {
  if (!isReady()) return false;
  printerInit();
  bool any=false;
  auto sendLine=[&](const String& s){ String line=toASCII(s)+"\n"; bool ok=_sendBytesChecked(printerSerial_, (const uint8_t*)line.c_str(), line.length(), "LINE"); any = any || ok; };
  sendLine("==============================");
  sendLine(toASCII(od.storeName));
  sendLine("==============================");
  sendLine("");
  
  // 注文番号を特大表示（GS ! n で 2倍幅×2倍高）
  const uint8_t D_W_H[] = {0x1D, 0x21, 0x11};  // GS ! 0x11 (2x width, 2x height)
  const uint8_t D_RESET[] = {0x1D, 0x21, 0x00}; // GS ! 0x00 (reset to normal)
  _sendBytesChecked(printerSerial_, D_W_H, sizeof(D_W_H), "GS ! 0x11 (double size)");
  sendLine("Order No. " + toASCII(od.orderNo));
  _sendBytesChecked(printerSerial_, D_RESET, sizeof(D_RESET), "GS ! 0x00 (reset size)");
  
  sendLine("------------------------------");
  const size_t n = od.itemsRomaji.size();
  for (size_t i=0; i<n; ++i) {
    String name = (i < od.itemsRomaji.size()) ? od.itemsRomaji[i] : String("-");
    int qty     = (i < od.quantities.size())  ? od.quantities[i]  : 1;
    int unit    = (i < od.prices.size())      ? od.prices[i]      : 0;
    if (name.length() > 24) name = name.substring(0, 24);
    sendLine(name);
    sendLine("  x" + String(qty) + "   " + String(unit) + "yen");
  }
  sendLine("------------------------------");
  sendLine("Total: " + String(od.totalAmount) + " yen");
  sendLine(isTimeValid() ? getCurrentDateTime() : "Time not synced");
  sendLine(toASCII(od.footerMessage));
  sendFeedLines(4);
  sendCutCommand();
  return any; // 少なくとも1行送れていなければ false
}

// Order から英語レシート用 PrintOrderData を生成して既存実装へ委譲
bool PrinterRenderer::printReceiptEN(const Order& order) {
  if (!isReady()) return false;

  PrintOrderData od;
  od.orderNo       = order.orderNo;
  od.storeName     = S().settings.store.nameRomaji; // 英語店名（ローマ字）
  od.footerMessage = "Thank you!";
  od.dateTime      = isTimeValid() ? getCurrentDateTime() : "Time not synced";

  int total = 0;
  for (const auto& it : order.items) {
    // 調整行（ちんちろ等）も印刷対象に含める
    String romaji = it.name;
    
    // ADJUST以外はメニューからローマ字名を引く
    if (it.kind != "ADJUST") {
      for (const auto& m : S().menu) {
        if (m.sku == it.sku || m.name == it.name) { romaji = m.nameRomaji; break; }
      }
    }

    int unit = (it.unitPriceApplied > 0) ? it.unitPriceApplied : it.unitPrice;
    int qty  = it.qty > 0 ? it.qty : 1;
    int sub  = unit * qty - (it.discountValue > 0 ? it.discountValue : 0);

    od.itemsRomaji.push_back(romaji);
    od.quantities.push_back(qty);
    od.prices.push_back(unit);
    total += sub;
  }
  od.totalAmount = total;

  return printReceiptEN(od); // 既存 PrintOrderData 版へ
}

// =============================================================================
// 超ミニ疎通テスト（ASCIIのみ / ビットマップ未使用 / 大量HEXダンプ）
// =============================================================================
bool PrinterRenderer::printHelloWorldTest() {
  if (!isReady()) { Serial.println("[PRINT] NG: not initialized"); return false; }
  Serial.println("========== HELLO TEST START ==========");
  Serial.printf("[PRINT] UART: %d bps (ESP32 RX=%d, TX=%d)\n", baud_, PRN_RX, PRN_TX);
  printerInit(); // 既定初期化（再begin含む）
  bool any=false;
  auto sendAscii=[&](const char* s){ String line=String(s)+"\n"; bool ok=_sendBytesChecked(printerSerial_, (const uint8_t*)line.c_str(), line.length(), "HELLO"); any = any || ok; };
  sendAscii("HELLO WORLD");
  sendAscii("hallo warld");
  sendAscii("1234567890 !@#$%^&*()-_+=");
  sendAscii("If you can read this, UART OK.");
  sendFeedLines(4);
  sendCutCommand();
  Serial.println("=========== HELLO TEST END ===========");
  return any;
}
