#include <M5Unified.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <sntp.h>
#include "ws_hub.h"
#include "server_routes.h"
#include "store.h"
#include "printer_queue.h"
#include "printer_render.h"

// WiFi設定
const char* ap_ssid = "KDS-ESP32";
const char* ap_password = "kds-2025";

// 家庭用WiFi設定（NTP時刻同期用）
const char* sta_ssid = "";     // 家庭用WiFi SSIDを設定
const char* sta_password = ""; // 家庭用WiFiパスワードを設定

// NTPサーバー設定
const char* ntp_server1 = "ntp.nict.jp";
const char* ntp_server2 = "time.google.com";
const char* ntp_server3 = "pool.ntp.org";

AsyncWebServer server(80);

// 時刻同期関数
bool syncTimeWithNTP() {
    Serial.println("[TIME] NTP時刻同期開始");
    
    // NTPサーバー設定
    configTime(9 * 3600, 0, ntp_server1, ntp_server2, ntp_server3); // JST = UTC+9
    
    // 時刻同期待機（10秒タイムアウト）
    int timeout = 10;
    while (timeout > 0) {
        time_t now = time(nullptr);
        if (now > 1000000000) { // 有効なタイムスタンプ
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            Serial.printf("[TIME] NTP時刻同期成功: %04d/%02d/%02d %02d:%02d:%02d\n",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return true;
        }
        delay(1000);
        timeout--;
        Serial.print(".");
    }
    
    Serial.println("\n[TIME] NTP時刻同期タイムアウト");
    return false;
}

// 現在時刻をフォーマットした文字列で取得
String getCurrentDateTime() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

// 時刻が有効かチェック
bool isTimeValid() {
    time_t now = time(nullptr);
    return now > 1000000000; // 2001年以降なら有効
}

void setup() {
    // M5Stack ATOM 初期化
    M5.begin();
    
    Serial.begin(115200);
    Serial.println("KDS システム起動中...");
    
    // タイムゾーンを日本時間に設定
    setenv("TZ", "JST-9", 1);
    tzset();
    Serial.println("タイムゾーン設定: JST-9");
    
    // WiFi接続試行（時刻同期用）
    bool wifi_connected = false;
    if (strlen(sta_ssid) > 0) {
        Serial.printf("[WIFI] STAモードで%sに接続試行...\n", sta_ssid);
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(sta_ssid, sta_password);
        
        int wifi_timeout = 10; // 10秒タイムアウト
        while (WiFi.status() != WL_CONNECTED && wifi_timeout > 0) {
            delay(1000);
            Serial.print(".");
            wifi_timeout--;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            wifi_connected = true;
            Serial.printf("\n[WIFI] STA接続成功: %s\n", WiFi.localIP().toString().c_str());
            
            // NTP時刻同期実行
            syncTimeWithNTP();
        } else {
            Serial.println("\n[WIFI] STA接続失敗、APモードのみで継続");
        }
    }
    
    // APモードで起動（必須）
    if (!wifi_connected) {
        WiFi.mode(WIFI_AP);
    }
    WiFi.softAP(ap_ssid, ap_password);
    
    // LittleFS 初期化
    if (!LittleFS.begin()) {
        Serial.println("LittleFS マウント失敗");
        return;
    }
    Serial.println("LittleFS マウント成功");
    
    // データストレージ初期化（スナップショット復元 + 初期メニュー）
    if (!snapshotLoad()) {
        Serial.println("スナップショット読込に失敗、初期メニューで開始");
    }
    ensureInitialMenu();
    
    IPAddress apIP = WiFi.softAPIP();
    Serial.println("=== KDS ESP32 起動完了 ===");
    Serial.printf("AP SSID: %s\n", ap_ssid);
    Serial.printf("AP IP: %s\n", apIP.toString().c_str());
    Serial.printf("アクセスURL: http://%s/\n", apIP.toString().c_str());
    if (wifi_connected) {
        Serial.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("時刻同期: %s\n", isTimeValid() ? "有効" : "無効");
        Serial.printf("現在時刻: %s\n", getCurrentDateTime().c_str());
    }
    
    // WebSocket初期化
    initWsHub(server);
    
    // HTTP API ルート初期化
    initHttpRoutes(server);
    
    // 静的ファイル配信（PWA）
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    
    // サーバー開始
    server.begin();
    Serial.println("WebServer 開始");
    
    // ATOM Printerキット記事推奨初期化
    extern HardwareSerial printerSerial;
    Serial.println("ATOM Printerキット記事仕様で準備中...");
    
    // Serial2事前設定クリア
    printerSerial.end();
    delay(200);
    
    // プリンタレンダラー初期化 (記事仕様)
    if (g_printerRenderer.initialize(&printerSerial)) {
        Serial.println("[PRINT] ATOM Printer renderer initialized - article specs");
        
        // T5. 起動時に完全初期化を実行 (記事推奨手順)
        g_printerRenderer.printerInit();
        Serial.println("起動時プリンタ記事仕様初期化完了");
    } else {
        Serial.println("警告: プリンタレンダラー初期化失敗");
    }
    
    Serial.println("セットアップ完了");
}

void loop() {
    // メインループ処理
    M5.update();
    
    // 印刷キュー処理
    tickPrintQueue();
    
    delay(10);
}