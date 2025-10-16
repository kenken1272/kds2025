#pragma once
#include <vector>
#include <WString.h>
#include <ArduinoJson.h>
#include <stdint.h>

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

struct SalesSummary {
    uint32_t confirmedOrders{0};
    uint32_t cancelledOrders{0};
    int64_t revenue{0};
    int64_t cancelledAmount{0};
    uint32_t lastUpdated{0};
};

State& S();

const SalesSummary& getSalesSummary();
bool loadSalesSummary();
bool saveSalesSummary();
bool recalculateSalesSummary();
void applyOrderToSalesSummary(const Order& order);
void applyCancellationToSalesSummary(const Order& order);

using ArchiveOrderVisitor = bool (*)(const Order&, const String&, uint32_t archivedAt, void* context);

String allocateOrderNo();
String generateSkuMain();
String generateSkuSide();

Order* findOrderByNo(const String& orderNo);
int computeOrderTotal(const Order& order);
void orderToJson(JsonObject json, const Order& order);
bool orderFromJson(JsonVariantConst json, Order& order);
size_t estimateOrderDocumentCapacity(const Order& order);

bool archiveAppend(const Order& order, const String& sessionId, uint32_t archivedAt);
bool archiveOrderAndRemove(const String& orderNo, const String& sessionId, uint32_t archivedAt = 0, bool logWal = true);
bool archiveForEach(const String& sessionIdFilter, ArchiveOrderVisitor visitor, void* context);
bool archiveFindOrder(const String& sessionIdFilter, const String& orderNo, Order& outOrder, uint32_t* archivedAt = nullptr);
bool archiveReplaceOrder(const Order& order, const String& sessionId, uint32_t archivedAt);

bool snapshotSave();
bool snapshotLoad();
void requestSnapshotSave();
bool consumeSnapshotSaveRequest();
bool walAppend(const String& line);
bool recoverToLatest(String &outLastTs);
bool getLatestSnapshotJson(String& outJson, String& outPath);

void ensureInitialMenu();
void forceCreateInitialMenu();
void createInitialMenuItems();