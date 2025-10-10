#pragma once
#include <vector>
#include <WString.h>

// ========== 基本 ==========
struct Session {
    String sessionId;      // 例: "2025-09-25-AM"
    uint32_t startedAt{0};
    bool exported{false};
    uint16_t nextOrderSeq{1}; // 1..9999
};

struct PrinterState {
    bool paperOut{false};
    bool overheat{false};
    uint16_t holdJobs{0}; // 紙切れ時にホールドしたジョブ数
};

// category: "MAIN" | "SIDE"
struct MenuItem {
    String sku;
    String name;
    String nameRomaji;             // ローマ字表記（レシート印刷用）
    String category;               // MAIN or SIDE
    bool active{true};
    // MAIN用
    int price_normal{0};
    int price_presale{0};          // 0 の場合は未設定扱い
    int presale_discount_amount{0}; // -100 等
    // SIDE用
    int price_single{0};
    int price_as_side{0};
};

struct Chinchiro {
    bool enabled{false};
    // SET合計に倍率適用
    // 例: [0, 0.5, 1, 2, 3]
    std::vector<float> multipliers;
    String rounding{"round"}; // round|floor|ceil
};

struct Settings {
    uint32_t catalogVersion{1};
    Chinchiro chinchiro;
    struct { uint16_t min{1}; uint16_t max{9999}; } numbering;
    struct { 
        String name{"KDS BURGER"}; 
        String registerId{"REG-01"};
        String nameRomaji{"KDS BURGER"};  // ローマ字表記（レシート印刷用）
    } store;
    bool presaleEnabled{true}; // 前売り機能ON/OFF
    struct {
        bool enabled{false};    // QRコード印刷ON/OFF
        String content{""};     // QRコード内容（URL等）
    } qrPrint;
};

// ========== 注文 ==========
struct LineItem {
    String sku;   // main_0001 / side_0001 等
    String name;
    int qty{1};
    // スナップショット（販売時点の確定値）
    int unitPriceApplied{0};  // ←確定単価
    String priceMode;         // "normal"|"presale"|""(side)
    String kind;              // "MAIN"|"SIDE_AS_SET"|"SIDE_SINGLE"|"ADJUST"
    // 互換（UI・CSV利便用）
    int unitPrice{0};         // = unitPriceApplied
    String discountName;      // 追加割引名（今回は空）
    int discountValue{0};     // 追加割引額（今回は0）
};

struct Order {
    String orderNo;           // "0042"
    String status;            // COOKING|DONE|CANCELLED
    uint32_t ts{0};           // epoch秒
    bool printed{false};
    bool cooked{false};       // 調理済みフラグ
    bool pickup_called{false}; // 呼び出し中（呼び出し画面に表示中）
    bool picked_up{false};    // 品出し済み（受け渡し完了）
    String cancelReason;
    std::vector<LineItem> items;
};

struct State {
    Settings settings;
    Session session;
    PrinterState printer;
    std::vector<MenuItem> menu;   // MAIN + SIDE
    std::vector<Order> orders;    // NEWは含めない
};

// ========== グローバルアクセス ==========
State& S();

// ========== NVS採番 ==========
String allocateOrderNo();        // 4桁ゼロ埋め（1..9999, 衝突回避）
String generateSkuMain();        // "main_0001"
String generateSkuSide();        // "side_0001"

// ========== スナップショット / WAL ==========
bool snapshotSave();             // A/Bローテ + fsync
bool snapshotLoad();             // 最新A/Bをロード
bool walAppend(const String& line);  // "TS,OP,JSON"
bool recoverToLatest(String &outLastTs); // snapshot + WAL適用

// ========== 初期データ投入 ==========
void ensureInitialMenu();        // A/B/Cバーガー & ドリンクA-D, ポテトS
void forceCreateInitialMenu();   // メニューを強制的に再作成（デバッグ用）
void createInitialMenuItems();   // 実際のメニューアイテム作成ロジック