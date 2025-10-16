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
        if (enableAccessPoint()) {
            g_apResumeScheduled = false;
        } else {
            Serial.println("[E] ap resume failed");
            g_apResumeAtMs = now + 5000;
        }
    }
}

bool disableAccessPointFor(uint32_t resumeDelayMs) {
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

static void rotateWalAfterSnapshot() {
    if (!LittleFS.exists("/kds/wal.log")) {
        return;
    }

    uint32_t epoch = time(nullptr);
    String archiveName = "/kds/wal." + String(epoch) + ".log";

    if (!LittleFS.rename("/kds/wal.log", archiveName.c_str())) {
        Serial.println("[E] wal rotate failed");
        return;
    }

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

    std::sort(walFiles.begin(), walFiles.end(), std::greater<String>());
    for (size_t i = 2; i < walFiles.size(); ++i) {
        LittleFS.remove(walFiles[i].c_str());
    }
}

static bool performSnapshot(const char* label) {
    (void)label;
    if (snapshotSave()) {
        rotateWalAfterSnapshot();
        return true;
    }

    Serial.println("[E] snapshot failed");
    return false;
}

bool enableAccessPoint() {
    wifi_mode_t desired = WiFi.isConnected() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    WiFi.mode(desired);
    WiFi.enableAP(true);
    // Allow up to 8 tablets to stay connected simultaneously (ESP32 maximum)
    bool ok = WiFi.softAP(ap_ssid, ap_password, 1, 0, 8);
    if (ok) {
        g_apEnabled = true;
        Serial.println("[WiFi] AP started");
    } else {
        Serial.println("[E] ap start failed");
        g_apEnabled = false;
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
    configTime(9 * 3600, 0, ntp_server1, ntp_server2, ntp_server3);
    
    int timeout = 10;
    while (timeout > 0) {
        time_t now = time(nullptr);
        if (now > 1000000000) {
            Serial.println("[NTP] ok");
            return true;
        }
        delay(1000);
        timeout--;
    }
    Serial.println("[E] ntp sync failed");
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
    Serial.println("[BOOT] ok");
    
    setenv("TZ", "JST-9", 1);
    tzset();
    
    bool wifi_connected = false;
    if (strlen(sta_ssid) > 0) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(sta_ssid, sta_password);
        
        int wifi_timeout = 10;
        while (WiFi.status() != WL_CONNECTED && wifi_timeout > 0) {
            delay(1000);
            wifi_timeout--;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            wifi_connected = true;
            syncTimeWithNTP();
        }
    }
    
    if (!wifi_connected) {
        WiFi.mode(WIFI_AP);
    }
    WiFi.setSleep(false);
    WiFi.softAPsetHostname("kds");
    if (WiFi.softAP(ap_ssid, ap_password, 1, 0, 8)) {
        g_apEnabled = true;
        Serial.println("[WiFi] AP started");
    } else {
        Serial.println("[E] ap start failed");
    }
    g_apResumeScheduled = false;
    
    if (!LittleFS.begin()) {
        Serial.println("[E] fs mount failed");
        return;
    }
    
    if (!snapshotLoad()) {
        Serial.println("[E] snapshot load failed");
    }
    ensureInitialMenu();

    if (!loadSalesSummary()) {
        Serial.println("[E] sales summary init failed");
    }
    
    initWsHub(server);
    
    initHttpRoutes(server);
    
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    
    server.begin();
    
    extern HardwareSerial printerSerial;
    printerSerial.end();
    delay(200);
    
    if (g_printerRenderer.initialize(&printerSerial)) {
        g_printerRenderer.printerInit();
    } else {
        Serial.println("[E] printer renderer init failed");
    }
}

void loop() {
    M5.update();
    
    tickPrintQueue();
    processPendingAccessPointTasks();
    
    static uint32_t lastSnapshotMs = 0;
    if (consumeSnapshotSaveRequest()) {
        performSnapshot("即時リクエスト");
        lastSnapshotMs = millis();
    }

    if (millis() - lastSnapshotMs >= 30000) {
        performSnapshot("30秒タイマー");
        lastSnapshotMs = millis();
    }
    
    delay(10);
}