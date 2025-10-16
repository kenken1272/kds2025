#include <M5Unified.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <sntp.h>
#include <vector>
#include <algorithm>
#include "ws_hub.h"
#include "server_routes.h"
#include "store.h"
#include "printer_queue.h"
#include "printer_render.h"

const char* ap_ssid = "KDS-ESP32";
const char* ap_password = "kds-2025";

const char* sta_ssid = "";
const char* sta_password = "";

const char* ntp_server1 = "ntp.nict.jp";
const char* ntp_server2 = "time.google.com";
const char* ntp_server3 = "pool.ntp.org";

AsyncWebServer server(80);

static bool g_apEnabled = false;
static bool g_apResumeScheduled = false;
static uint32_t g_apResumeAtMs = 0;
static bool g_apDisablePending = false;
static uint32_t g_apDisableResumeDelayMs = 0;

static void scheduleApDisable(uint32_t resumeDelayMs) {
    g_apDisableResumeDelayMs = resumeDelayMs;
    g_apDisablePending = true;
}

static bool enableAccessPoint();

static void pollAccessPointResume() {
    if (!g_apResumeScheduled) {
        return;
    }

    uint32_t now = millis();
    if (static_cast<int32_t>(now - g_apResumeAtMs) >= 0) {
        Serial.println("[WIFI] AP resume timer fired");
        if (enableAccessPoint()) {
            Serial.println("[WIFI] AP resumed");
            g_apResumeScheduled = false;
        } else {
            Serial.println("[WIFI] AP resume failed, retrying in 5s");
            g_apResumeAtMs = now + 5000;
        }
    }
}

bool disableAccessPointFor(uint32_t resumeDelayMs) {
    Serial.println("[WIFI] Disabling access point");
    WiFi.softAPdisconnect(true);
    WiFi.enableAP(false);
    wifi_mode_t current = WiFi.getMode();
    if (current == WIFI_MODE_AP) {
        WiFi.mode(WIFI_MODE_NULL);
    } else if (current == WIFI_MODE_APSTA) {
        WiFi.mode(WIFI_MODE_STA);
    }
    g_apEnabled = false;

    if (resumeDelayMs > 0) {
        g_apResumeScheduled = true;
        g_apResumeAtMs = millis() + resumeDelayMs;
    } else {
        g_apResumeScheduled = false;
    }

    return true;
}

static void processPendingAccessPointTasks() {
    if (g_apDisablePending) {
        g_apDisablePending = false;
        disableAccessPointFor(g_apDisableResumeDelayMs);
    }
    pollAccessPointResume();
}

bool enableAccessPoint() {
    wifi_mode_t desired = WiFi.isConnected() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    WiFi.mode(desired);
    WiFi.enableAP(true);
    bool ok = WiFi.softAP(ap_ssid, ap_password);
    if (ok) {
        g_apEnabled = true;
    }
    return ok;
}

bool isAccessPointEnabled() {
    return g_apEnabled;
}

bool isAccessPointResumeScheduled() {
    return g_apResumeScheduled;
}

uint32_t getAccessPointResumeEtaMs() {
    return g_apResumeScheduled ? g_apResumeAtMs : 0;
}

void requestAccessPointSuspend(uint32_t resumeDelayMs) {
    scheduleApDisable(resumeDelayMs);
}

bool syncTimeWithNTP() {
    Serial.println("[TIME] NTP時刻同期開始");
    
    configTime(9 * 3600, 0, ntp_server1, ntp_server2, ntp_server3);
    
    int timeout = 10;
    while (timeout > 0) {
        time_t now = time(nullptr);
        if (now > 1000000000) {
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

String getCurrentDateTime() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

bool isTimeValid() {
    time_t now = time(nullptr);
    return now > 1000000000;
}

void setup() {
    M5.begin();
    
    Serial.begin(115200);
    Serial.println("KDS システム起動中...");
    
    setenv("TZ", "JST-9", 1);
    tzset();
    Serial.println("タイムゾーン設定: JST-9");
    
    bool wifi_connected = false;
    if (strlen(sta_ssid) > 0) {
        Serial.printf("[WIFI] STAモードで%sに接続試行...\n", sta_ssid);
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(sta_ssid, sta_password);
        
        int wifi_timeout = 10;
        while (WiFi.status() != WL_CONNECTED && wifi_timeout > 0) {
            delay(1000);
            Serial.print(".");
            wifi_timeout--;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            wifi_connected = true;
            Serial.printf("\n[WIFI] STA接続成功: %s\n", WiFi.localIP().toString().c_str());
            
            syncTimeWithNTP();
        } else {
            Serial.println("\n[WIFI] STA接続失敗、APモードのみで継続");
        }
    }
    
    if (!wifi_connected) {
        WiFi.mode(WIFI_AP);
    }
    WiFi.softAP(ap_ssid, ap_password);
    g_apEnabled = true;
    g_apResumeScheduled = false;
    
    if (!LittleFS.begin()) {
        Serial.println("LittleFS マウント失敗");
        return;
    }
    Serial.println("LittleFS マウント成功");
    
    if (!snapshotLoad()) {
        Serial.println("スナップショット読込に失敗、初期メニューで開始");
    }
    ensureInitialMenu();

    if (!loadSalesSummary()) {
        Serial.println("[SALES] サマリ初期化に失敗しました");
    }
    
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
    
    initWsHub(server);
    
    initHttpRoutes(server);
    
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    
    server.begin();
    Serial.println("WebServer 開始");
    
    extern HardwareSerial printerSerial;
    Serial.println("ATOM Printerキット記事仕様で準備中...");
    
    printerSerial.end();
    delay(200);
    
    if (g_printerRenderer.initialize(&printerSerial)) {
        Serial.println("[PRINT] ATOM Printer renderer initialized - article specs");
        
        g_printerRenderer.printerInit();
        Serial.println("起動時プリンタ記事仕様初期化完了");
    } else {
        Serial.println("警告: プリンタレンダラー初期化失敗");
    }
    
    Serial.println("セットアップ完了");
}

void loop() {
    M5.update();
    
    tickPrintQueue();
    processPendingAccessPointTasks();
    
    // 30秒ごとのスナップショット
    static uint32_t lastSnapshotMs = 0;
    if (millis() - lastSnapshotMs >= 30000) {
        Serial.println("[SNAPSHOT] 30秒タイマー: スナップショット保存開始");
        
        if (snapshotSave()) {
            Serial.println("[SNAPSHOT] スナップショット保存成功");
            
            // WALローテーション
            if (LittleFS.exists("/kds/wal.log")) {
                uint32_t epoch = time(nullptr);
                String archiveName = "/kds/wal." + String(epoch) + ".log";
                
                if (LittleFS.rename("/kds/wal.log", archiveName.c_str())) {
                    Serial.printf("[WAL] ローテーション完了: %s\n", archiveName.c_str());
                    
                    // 古いWALファイルを削除（最新2世代のみ保持）
                    File root = LittleFS.open("/kds");
                    std::vector<String> walFiles;
                    while (File file = root.openNextFile()) {
                        String fname = String(file.name());
                        if (fname.startsWith("wal.") && fname.endsWith(".log")) {
                            walFiles.push_back("/kds/" + fname);
                        }
                        file.close();
                    }
                    root.close();
                    
                    // ファイル名でソート（新しい順）
                    std::sort(walFiles.begin(), walFiles.end(), std::greater<String>());
                    
                    // 3つ目以降を削除
                    for (size_t i = 2; i < walFiles.size(); i++) {
                        if (LittleFS.remove(walFiles[i].c_str())) {
                            Serial.printf("[WAL] 古いファイル削除: %s\n", walFiles[i].c_str());
                        }
                    }
                } else {
                    Serial.println("[WAL] エラー: ローテーション失敗");
                }
            }
        } else {
            Serial.println("[SNAPSHOT] 警告: スナップショット保存失敗");
        }
        
        lastSnapshotMs = millis();
    }
    
    delay(10);
}