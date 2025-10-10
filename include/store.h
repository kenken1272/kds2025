#pragma once
#include <vector>
#include <WString.h>

struct Session {
    String sessionId;
    uint32_t startedAt{0};
    bool exported{false};
    uint16_t nextOrderSeq{1};
};

struct PrinterState {
    bool paperOut{false};
    bool overheat{false};
    uint16_t holdJobs{0};
};

struct MenuItem {
    String sku;
    String name;
    String nameRomaji;
    String category;
    bool active{true};
    int price_normal{0};
    int price_presale{0};
    int presale_discount_amount{0};
    int price_single{0};
    int price_as_side{0};
};

struct Chinchiro {
    bool enabled{false};
    std::vector<float> multipliers;
    String rounding{"round"};
};

struct Settings {
    uint32_t catalogVersion{1};
    Chinchiro chinchiro;
    struct { uint16_t min{1}; uint16_t max{9999}; } numbering;
    struct { 
        String name{"KDS BURGER"}; 
        String registerId{"REG-01"};
        String nameRomaji{"KDS BURGER"};
    } store;
    bool presaleEnabled{true};
    struct {
        bool enabled{false};
        String content{""};
    } qrPrint;
};

struct LineItem {
    String sku;
    String name;
    int qty{1};
    int unitPriceApplied{0};
    String priceMode;
    String kind;
    int unitPrice{0};
    String discountName;
    int discountValue{0};
};

struct Order {
    String orderNo;
    String status;
    uint32_t ts{0};
    bool printed{false};
    bool cooked{false};
    bool pickup_called{false};
    bool picked_up{false};
    String cancelReason;
    std::vector<LineItem> items;
};

struct State {
    Settings settings;
    Session session;
    PrinterState printer;
    std::vector<MenuItem> menu;
    std::vector<Order> orders;
};

State& S();

String allocateOrderNo();
String generateSkuMain();
String generateSkuSide();

bool snapshotSave();
bool snapshotLoad();
bool walAppend(const String& line);
bool recoverToLatest(String &outLastTs);

void ensureInitialMenu();
void forceCreateInitialMenu();
void createInitialMenuItems();