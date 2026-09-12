// =====================================================================
//  Traders Refuse Irrelevant Items - KenshiLib native port
//  =====================================================================
//  The plugin owns both a value cue on the player's sell side and hard
//  refusal in the inventory add/placement paths that fire for right-click
//  and drag/drop sales.
//
//  Acceptance is driven by the shop's real vendor lists first:
//    - itemFunction (+ inventorySection substring) -> category
//    - FCS VENDOR_LIST refs on the shopkeeper, squad, or building -> stock
//    - exact stock stringID matches -> full acceptance
//    - special cases: off-list food at reduced value, crossbow shops buy ammo,
//      shop inputs, and the FCS force-allow list for general trade stores
//    - no vendor list found -> fail open to vanilla-style acceptance
//
//  Build: Release | x64 | v100, against KENSHILIB_DIR + Boost 1.60.
//
//  Category enumerators are CAT_*-prefixed on purpose: Kenshi's Enums.h
//  defines unscoped globals WEAPON/ARMOUR/WATER/BLUEPRINT/ARTIFACTS, and
//  v100 is C++03 (no enum class), so an unprefixed enum here collides.
//
//  Note on crash-safety: SEH __try/__except cannot coexist with C++ objects
//  that need unwinding (std::string/std::set) in the same function (MSVC
//  C2712), so - like the stock KenshiLib examples - guarded wrappers stay
//  small and the main logic relies on strict null checks against known-typed
//  pointers.
// =====================================================================

#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/ShopTraderInventory.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/Item.h>
#include <kenshi/Inventory.h>
#include <kenshi/Character.h>
#include <kenshi/Platoon.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/RaceData.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Enums.h>

#include <string>
#include <map>
#include <set>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// ---------------------------------------------------------------------
//  CONFIG  (compile-time toggles for the native plugin)
// ---------------------------------------------------------------------
namespace cfg
{
    static const bool   enabled             = true;   // master switch
    static const bool   debug               = true;   // [TRII] lines to RE_Kenshi debug log
    static const bool   observeOnly         = false;  // log decisions but never block/reprice
    static const bool   enableValueHook     = true;   // 0/reduced/full value cue on the sell side
    static const bool   enableAddItemHooks  = true;   // install/log broad inventory-add transfer paths
    static const bool   enableAddItemRefusal = true;  // refuse only proven player sales into the active shopkeeper
    static const bool   enablePlaceItemFromMouseHook = true;   // install/log GUI drag/drop placement path
    static const bool   enablePlaceItemFromMouseRefusal = true; // block refused drag/drop sales before placement
    static const bool   enableTransferMouseItemHook = true;   // install/log drag/drop transfer path
    static const bool   enableTransferMouseItemRefusal = true; // block refused drag/drop sales at the mouse transfer
    static const bool   enableSectionPlaceHook = true;   // install/log direct section grid placement
    static const bool   enableSectionPlaceRefusal = true; // block refused drag/drop sales at the grid placement
    static const bool   restoreRefusedDragDropItems = true; // section placement fires after pickup, so hand refused items back

    static const bool   payFullLocalPrice   = true;   // pay avg (local) price for accepted goods
    static const bool   playerSellValue     = true;   // getValueSingle(isPlayer) value that marks the sell side
    static const int    refusedPreviewValue = 0;      // TEST: was 1 (day-one assumption that "zero looked crash-prone" -
                                                      // no commit ever verified it). 0 stops the money leak on refused
                                                      // sales; if it CTDs, suspect getMaxAffordableNum div-by-zero on the
                                                      // sell-all path (RVA 0x75C3B0), not the preview itself.
    static const bool   refuseUnknown       = false;  // false => allow unclassified (OTHER) items
    static const bool   useStockAcceptance  = true;   // the shop's real stock expands acceptance
    static const bool   strictVendorStockAcceptance = true; // stock categories describe shops; exact IDs approve sales
    static const bool   enableWeaponTierAcceptance = true; // weapon shops buy any weapon at/above their lowest stocked
                                                      // manufacturer "model value" tier, not just exact-stocked IDs
    static const bool   scanAllVendorRefs   = true;   // inspect FCS VENDOR_LIST refs on squad/shop/building data
    static const bool   acceptAllWhenNoVendorList = true; // missing vendor data means leave vanilla trading alone
    static const double offListFoodMult     = 0.6;    // most shops will still buy food, just at a worse price
    static const bool   logVendorListItems  = true;   // dump exact FCS vendor-list stock refs once per run
    static const int    maxVendorItemLogs   = 240;    // enough to learn shops, bounded to avoid log floods
    static const uint32_t maxStockScan      = 4096;   // per-category cap when folding vendor/live stock into the shop
                                                      // profile. The old 80-120 caps truncated large pools - e.g. the
                                                      // Great Library's research "items" list - so off-list research
                                                      // artifacts past the cap were never added to stockIds and got
                                                      // refused. 4096 is a defensive ceiling well above any real list.
    static const bool   requireMoneyTrade   = true;   // only act inside real money trades
    static const bool   treatOwnerlessTradeItemsAsPlayerSales = true; // catches equipped/spawned items with null owners

    // "silent" (block only) | "vocal" (+ speech) | "eject" (+ kick out on repeat)
    static const char*  refusalMode         = "vocal";
    static const int    refusalSpeechCooldownMs = 900;      // local debounce for held-click refusal loops
}

// ---------------------------------------------------------------------
//  CATEGORIES  (CAT_-prefixed to avoid Kenshi Enums.h global collisions)
// ---------------------------------------------------------------------
enum Cat
{
    CAT_FOOD, CAT_BOOZE, CAT_WATER, CAT_WEAPON, CAT_RANGED, CAT_AMMO,
    CAT_ARMOUR, CAT_BACKPACK, CAT_MEDICAL, CAT_TOOL, CAT_BLUEPRINT, CAT_BOOK,
    CAT_DRUG, CAT_ROBOTICS, CAT_RAWMATS, CAT_BUILDMATS, CAT_TRADEGOODS,
    CAT_ARTIFACTS, CAT_MAP, CAT_OTHER
};

static const char* catLabel(Cat cat)
{
    switch (cat)
    {
    case CAT_FOOD: return "food";
    case CAT_BOOZE: return "booze";
    case CAT_WATER: return "water";
    case CAT_WEAPON: return "weapon";
    case CAT_RANGED: return "ranged";
    case CAT_AMMO: return "ammo";
    case CAT_ARMOUR: return "armour";
    case CAT_BACKPACK: return "backpack";
    case CAT_MEDICAL: return "medical";
    case CAT_TOOL: return "tool";
    case CAT_BLUEPRINT: return "blueprint";
    case CAT_BOOK: return "book";
    case CAT_DRUG: return "drug";
    case CAT_ROBOTICS: return "robotics";
    case CAT_RAWMATS: return "rawmats";
    case CAT_BUILDMATS: return "buildmats";
    case CAT_TRADEGOODS: return "tradegoods";
    case CAT_ARTIFACTS: return "artifacts";
    case CAT_MAP: return "map";
    default: return "other";
    }
}

// ItemFunction enum -> category  (values from kenshi/Enums.h)
static Cat funcToCat(ItemFunction fn, bool* mapped)
{
    *mapped = true;
    switch (fn)
    {
    case ITEM_FOOD:            return CAT_FOOD;
    case ITEM_FOOD_RESTRICTED: return CAT_FOOD;
    case ITEM_WEAPON:          return CAT_WEAPON;
    case ITEM_AMMO:            return CAT_AMMO;
    case ITEM_CLOTHING:        return CAT_ARMOUR;
    case ITEM_CONTAINER:       return CAT_BACKPACK;
    case ITEM_FIRSTAID:        return CAT_MEDICAL;
    case ITEM_MEDRIGGING:      return CAT_MEDICAL;
    case ITEM_TOOL:            return CAT_TOOL;
    case ITEM_BLUEPRINT:       return CAT_BLUEPRINT;
    case ITEM_BOOK:            return CAT_BOOK;
    case ITEM_NARCOTIC:        return CAT_DRUG;
    case ITEM_ROBOTREPAIR:     return CAT_ROBOTICS;
    default: break;
    }
    *mapped = false;
    return CAT_OTHER;
}

// inventorySection substring (case-insensitive) -> category.
struct SectionRule { const char* key; Cat cat; };
static const SectionRule kSectionToCat[] = {
    { "ammo", CAT_AMMO }, { "ammunition", CAT_AMMO }, { "bolt", CAT_AMMO },
    { "crossbow", CAT_RANGED }, { "weapon", CAT_WEAPON }, { "armour", CAT_ARMOUR },
    { "clothing", CAT_ARMOUR }, { "foodstuff", CAT_FOOD }, { "food", CAT_FOOD },
    { "drink", CAT_BOOZE }, { "building material", CAT_BUILDMATS }, { "raw material", CAT_RAWMATS },
    { "raw", CAT_RAWMATS }, { "material", CAT_RAWMATS }, { "trade good", CAT_TRADEGOODS },
    { "trade", CAT_TRADEGOODS }, { "commodit", CAT_TRADEGOODS }, { "artifact", CAT_ARTIFACTS },
    { "ancient", CAT_ARTIFACTS }, { "tool", CAT_TOOL }, { "medical", CAT_MEDICAL },
    { "robot", CAT_ROBOTICS }, { "map", CAT_MAP },
};

static std::string toLower(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (char)tolower((unsigned char)out[i]);
    return out;
}

static bool readable(const void* p, size_t n);
static GameData* itemGameData(InventoryItemBase* item);
static Cat referenceCategoryForItemID(const std::string& itemId);

static Cat gameDataTypeToCat(itemType type)
{
    switch (type)
    {
    case WEAPON: return CAT_WEAPON;
    case CROSSBOW: return CAT_RANGED;
    case ARMOUR: return CAT_ARMOUR;
    case CONTAINER: return CAT_BACKPACK;
    case BLUEPRINT: return CAT_BLUEPRINT;
    case ARTIFACTS: return CAT_ARTIFACTS;
    case MAP_ITEM: return CAT_MAP;
    case LIMB_REPLACEMENT: return CAT_ROBOTICS;
    default: return CAT_OTHER;
    }
}

static bool sectionToCat(const std::string& sectionLower, Cat* out)
{
    for (size_t i = 0; i < sizeof(kSectionToCat) / sizeof(kSectionToCat[0]); ++i)
    {
        if (sectionLower.find(kSectionToCat[i].key) != std::string::npos)
        {
            *out = kSectionToCat[i].cat;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
//  PER-ARCHETYPE RULES
// ---------------------------------------------------------------------
struct Rule
{
    std::set<Cat> full;
    std::map<Cat, double> reduced;
};

static std::map<std::string, Rule> gRules;

static void buildRules()
{
    Rule bar;            bar.full.insert(CAT_FOOD); bar.full.insert(CAT_BOOZE); bar.full.insert(CAT_WATER);
    Rule armoursmith;    armoursmith.full.insert(CAT_RAWMATS); armoursmith.full.insert(CAT_ARMOUR); armoursmith.full.insert(CAT_WEAPON);
                         armoursmith.reduced[CAT_FOOD] = 0.2;
    Rule weaponsmith;    weaponsmith.full.insert(CAT_RAWMATS); weaponsmith.full.insert(CAT_WEAPON); weaponsmith.full.insert(CAT_RANGED); weaponsmith.full.insert(CAT_AMMO);
                         weaponsmith.reduced[CAT_FOOD] = 0.2;
    Rule wandering;      wandering.full.insert(CAT_TRADEGOODS); wandering.full.insert(CAT_RAWMATS); wandering.full.insert(CAT_ARTIFACTS);
                         wandering.reduced[CAT_FOOD] = 0.2;
    Rule general;        general.full.insert(CAT_TRADEGOODS); general.full.insert(CAT_TOOL); general.full.insert(CAT_RAWMATS);
                         general.full.insert(CAT_BUILDMATS); general.full.insert(CAT_WEAPON); general.full.insert(CAT_ARMOUR);
                         general.reduced[CAT_FOOD] = 0.2;
    Rule medic;          medic.full.insert(CAT_MEDICAL); medic.full.insert(CAT_DRUG); medic.reduced[CAT_FOOD] = 0.2;
    Rule robotics;       robotics.full.insert(CAT_ROBOTICS); robotics.full.insert(CAT_RAWMATS); robotics.reduced[CAT_FOOD] = 0.2;
    Rule foodVendor;     foodVendor.full.insert(CAT_FOOD); foodVendor.full.insert(CAT_WATER);
    Rule drugDealer;     drugDealer.full.insert(CAT_DRUG); drugDealer.reduced[CAT_FOOD] = 0.2;

    Rule def; // permissive default until detection is tuned
    for (int c = CAT_FOOD; c <= CAT_OTHER; ++c) def.full.insert((Cat)c);

    gRules["bar"] = bar;
    gRules["armoursmith"] = armoursmith;
    gRules["weaponsmith"] = weaponsmith;
    gRules["wandering_trader"] = wandering;
    gRules["general_store"] = general;
    gRules["medic"] = medic;
    gRules["robotics"] = robotics;
    gRules["food_vendor"] = foodVendor;
    gRules["drug_dealer"] = drugDealer;
    gRules["_default"] = def;
}

static const Rule& ruleFor(const std::string& archetype)
{
    std::map<std::string, Rule>::const_iterator it = gRules.find(archetype);
    if (it != gRules.end()) return it->second;
    return gRules["_default"];
}

// trader-name substring -> archetype. Vendor lists are authoritative when
// present; name hints are a fallback for missing/odd shop data.
struct NameHint { const char* key; const char* archetype; };
static const NameHint kNameHints[] = {
    { "barman", "bar" }, { "bartender", "bar" }, { "bar ", "bar" },
    { "weapon", "weaponsmith" }, { "crossbow", "weaponsmith" }, { "ranger", "weaponsmith" },
    { "armour", "armoursmith" }, { "armor", "armoursmith" },
    { "skeleton doctor", "robotics" }, { "robot", "robotics" },
    { "medic", "medic" }, { "doctor", "medic" }, { "surgeon", "medic" },
    { "drug", "drug_dealer" }, { "hashish", "drug_dealer" },
    { "food", "food_vendor" }, { "farm", "food_vendor" },
    { "trade goods", "general_store" },
    { "general", "general_store" }, { "travel", "general_store" }, { "adventur", "general_store" },
};

// ---------------------------------------------------------------------
//  SAFE-ish ACCESSORS  (null-guarded; typed pointers, no SEH)
// ---------------------------------------------------------------------
static std::string safeName(RootObject* obj)
{
    if (!obj) return std::string();
    return obj->getName();
}

static bool isTraderPlatoon(Character* shop)
{
    if (!shop) return false;
    ActivePlatoon* plat = shop->getPlatoon();
    if (!plat) return false;
    if (plat->getIsTrader()) return true;
    if (plat->getHasVendorList()) return true;
    if (plat->getHasSpecialItemsList()) return true;
    return false;
}

// ---------------------------------------------------------------------
//  ITEM CLASSIFICATION
// ---------------------------------------------------------------------
// Base valuation path only sees InventoryItemBase; use GameData type first,
// then itemFunction, then the inventorySection substring fallback.
static Cat classifyByFunctionAndSection(InventoryItemBase* item)
{
    if (!item) return CAT_OTHER;
    GameData* data = itemGameData(item);
    if (data)
    {
        Cat dc = gameDataTypeToCat(data->type);
        if (dc != CAT_OTHER) return dc;
        Cat rc = referenceCategoryForItemID(toLower(data->stringID));
        if (rc != CAT_OTHER) return rc;
    }

    bool mapped = false;
    Cat c = funcToCat(item->itemFunction, &mapped);
    if (mapped) return c;
    Cat sc;
    if (sectionToCat(toLower(item->inventorySection), &sc)) return sc;
    return CAT_OTHER;
}

// Full classifier for the block path, which has a concrete Item*.
static Cat classifyItem(Item* item)
{
    if (!item) return CAT_OTHER;
    GameData* data = itemGameData(item);
    if (data)
    {
        Cat dc = gameDataTypeToCat(data->type);
        if (dc != CAT_OTHER) return dc;
        Cat rc = referenceCategoryForItemID(toLower(data->stringID));
        if (rc != CAT_OTHER) return rc;
    }

    if (item->isCrossbow()) return CAT_RANGED;
    if (item->isArmour())   return CAT_ARMOUR;
    if (item->isWeapon())   return CAT_WEAPON;
    return classifyByFunctionAndSection(item);
}

// ---------------------------------------------------------------------
//  SHOP CONTEXT  (resolved lazily from the live trade, cached by pointer)
// ---------------------------------------------------------------------
struct ShopContext
{
    RootObject*    shop;
    std::string    archetype;
    bool           foundVendorList;
    bool           haveStock;
    std::set<Cat>  stockCats;
    std::set<std::string> stockIds;
    std::set<std::string> stockNames;
    std::set<GameData*> seenVendorLists;
    std::set<std::string> vendorListNames;
    bool           hasGeneralTradeAllowList;
    bool           isTradeShop;
    bool           sellsRobotics;
    bool           isWeaponsOnlyShop;
    bool           sellsWeapons;
    bool           sellsArmorOrClothing;
    bool           isBar;
    bool           isThiefFence;
    bool           isAcceptAllVendor;
    bool           isSkeletonVendor;
    bool           sellsFood;
    bool           sellsBlueprints;
    bool           hasWeaponModelTier;   // shop referenced >=1 weapon manufacturer with model values
    int            minWeaponModelValue;  // lowest manufacturer "model value" tier the shop stocks
    int            liveStockItems;
    int            templateStockRefs;

    ShopContext() : shop(NULL), archetype("_default"), foundVendorList(false), haveStock(false),
        hasGeneralTradeAllowList(false), isTradeShop(false), sellsRobotics(false),
        isWeaponsOnlyShop(false), sellsWeapons(false), sellsArmorOrClothing(false), isBar(false),
        isThiefFence(false), isAcceptAllVendor(false), isSkeletonVendor(false), sellsFood(false),
        sellsBlueprints(false), hasWeaponModelTier(false), minWeaponModelValue(0),
        liveStockItems(0), templateStockRefs(0) {}
    void clear()
    {
        shop = NULL; archetype = "_default"; foundVendorList = false; haveStock = false;
        stockCats.clear(); stockIds.clear(); stockNames.clear(); seenVendorLists.clear(); vendorListNames.clear();
        hasGeneralTradeAllowList = false; isTradeShop = false; sellsRobotics = false;
        isWeaponsOnlyShop = false; sellsWeapons = false; sellsArmorOrClothing = false; isBar = false;
        isThiefFence = false; isAcceptAllVendor = false; isSkeletonVendor = false; sellsFood = false;
        sellsBlueprints = false; hasWeaponModelTier = false; minWeaponModelValue = 0;
        liveStockItems = 0; templateStockRefs = 0;
    }
};

static ShopContext gCtx;

static std::string itemStringID(InventoryItemBase* item);
static bool isGeneralTradeAllowID(const std::string& itemId);
static void loadReferenceLists();
static bool setHasItemID(const std::set<std::string>& ids, const std::string& itemId);

struct ActiveTradeContext
{
    RootObject* player;
    RootObject* shop;
    std::set<Item*> playerItems;
    std::set<Item*> shopItems;

    ActiveTradeContext() : player(NULL), shop(NULL) {}
    void clear()
    {
        player = NULL;
        shop = NULL;
        playerItems.clear();
        shopItems.clear();
    }
};

static ActiveTradeContext gTrade;

static std::string classifyShop(RootObject* shop)
{
    if (!shop) return "_default";
    std::string name = toLower(safeName(shop));
    if (!name.empty())
    {
        for (size_t i = 0; i < sizeof(kNameHints) / sizeof(kNameHints[0]); ++i)
            if (name.find(kNameHints[i].key) != std::string::npos)
                return kNameHints[i].archetype;
    }
    return "_default";
}

static bool isCharacterType(RootObject* obj)
{
    if (!obj) return false;
    itemType t = obj->getDataType();
    return t == CHARACTER || t == HUMAN_CHARACTER || t == ANIMAL_CHARACTER;
}

static std::string normaliseName(const std::string& raw)
{
    std::string s = toLower(raw);
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    if (b == std::string::npos) return std::string();
    s = s.substr(b, e - b + 1);
    // collapse internal whitespace runs to single spaces
    std::string out;
    bool prevSpace = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char ch = s[i];
        bool sp = (ch == ' ' || ch == '\t');
        if (sp) { if (!prevSpace) out += ' '; prevSpace = true; }
        else    { out += ch; prevSpace = false; }
    }
    return out;
}

static bool characterIsSkeleton(Character* c)
{
    if (!readable(c, 8)) return false;
    RaceData* race = c->getRace();
    if (!readable(race, 8)) return false;
    if (race->robot) return true;

    GameData* data = race->data;
    if (!readable(data, 8)) return false;
    std::string key = toLower(data->name + " " + data->stringID);
    return key.find("skeleton") != std::string::npos ||
        key.find("robot") != std::string::npos;
}

static std::string stockCatSummary(const std::set<Cat>& cats)
{
    std::string out;
    for (std::set<Cat>::const_iterator it = cats.begin(); it != cats.end(); ++it)
    {
        if (!out.empty()) out += ",";
        out += catLabel(*it);
    }
    return out;
}

static std::string stockNameSummary(const std::set<std::string>& names)
{
    std::string out;
    int count = 0;
    for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it)
    {
        if (count++ >= 24) { out += ",..."; break; }
        if (!out.empty()) out += ",";
        out += *it;
    }
    return out;
}

static std::string vendorListSummary(const std::set<std::string>& names)
{
    std::string out;
    int count = 0;
    for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it)
    {
        if (count++ >= 12) { out += ",..."; break; }
        if (!out.empty()) out += ",";
        out += *it;
    }
    return out;
}

static bool stringInList(const std::string& value, const char* const* values, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (value == values[i])
            return true;
    return false;
}

static bool stockHasOnlyWeaponGoods(const std::set<Cat>& cats)
{
    bool hasWeaponGoods = false;
    for (std::set<Cat>::const_iterator it = cats.begin(); it != cats.end(); ++it)
    {
        if (*it == CAT_WEAPON || *it == CAT_RANGED || *it == CAT_AMMO)
        {
            hasWeaponGoods = true;
            continue;
        }
        return false;
    }
    return hasWeaponGoods;
}

static std::set<std::string> gGeneralTradeAllowIds;
static std::set<std::string> gFoodIds;
static std::set<std::string> gDrinkWaterIds;
static std::set<std::string> gBookIds;
static std::set<std::string> gUsefulBuildingMatIds;
static std::set<std::string> gCrossbowForceIds;
static std::set<std::string> gArmorForceIds;
static bool gReferenceListsLoaded = false;

// Identifies vendor lists that define goods general traders should accept even
// when those items are absent from the individual shop's vendor list.
static bool isGeneralTradeAllowList(GameData* vendor)
{
    if (!vendor) return false;
    std::string id = toLower(vendor->stringID);
    return id == "11-traders refuse irrelevant items.mod" ||
        id == "1013-gamedata.base" || id == "1386-gamedata.base";
}

static Cat referenceListCategoryHint(GameData* vendor)
{
    if (!vendor) return CAT_OTHER;
    std::string id = toLower(vendor->stringID);
    if (id == "43969-rebirth.mod") return CAT_FOOD;
    if (id == "12-traders refuse irrelevant items.mod") return CAT_BOOZE;
    if (id == "13-traders refuse irrelevant items.mod") return CAT_BOOK;
    if (id == "56360-rebirth.mod") return CAT_BUILDMATS;
    return CAT_OTHER;
}

// World/loot drops with no natural vendor list (unique boss CPUs, animal eggs,
// trophies, quest artifacts). The FCS force-accept lists omit these, so general
// traders refused them despite the "general stores buy world-only items" intent.
// Lowercase to match itemStringID(). Accepted only by trade shops (below).
static bool isLootOnlyTradeItem(const std::string& itemId)
{
    static const char* const kLootTradeItemIds[] = {
        "4029-gamedata.base",      // Beak Thing Egg
        "56099-newwworld.mod",     // Crab Egg
        "97662-rebirth.mod",       // Gurgler Egg
        "42338-changes_otto.mod",  // Leviathan Pearl
        "1533843-rebirth.mod",     // CPU of Cat-Lon
        "1533859-rebirth.mod",     // CPU of Cat-Lon (variant)
        "1533669-rebirth.mod",     // CPU of General Hat-12
        "1533665-rebirth.mod",     // CPU of General Jang
        "1533517-rebirth.mod",     // CPU of Rhinobot
        "1533516-rebirth.mod",     // CPU of the Head of Agriculture
        "1534121-__fixes.mod",     // Great White Claw
        "98510-rebirth.mod",       // Great White Skin
        "56122-rebirth.mod",       // Megacrab Ganglion
        "50984-rebirth.mod",       // Horn of the Megaraptor
        "57222-dialogue.mod",      // Chalice of Fire
        "1533445-dialogue.mod",    // Holy Seal
    };
    return stringInList(itemId, kLootTradeItemIds,
        sizeof(kLootTradeItemIds) / sizeof(kLootTradeItemIds[0]));
}

// Allows stores to buy obvious inputs for the things they sell, plus the
// mod-owned force-allow list for general trade stores. `item` (when supplied)
// enables the weapon manufacturer-tier check.
static bool isShopSpecificAcceptedGood(const std::string& itemId, Cat cat,
                                       InventoryItemBase* item)
{
    static const char* const roboticsInputIds[] = {
        "43395-changes_otto.mod", // Skeleton Muscle
        "43397-changes_otto.mod", // Motor
        "583-gamedata.base",      // Robotics Components
        "42164-gamedata.base",    // Electrical Components
        "579-gamedata.base",      // Steel Bars
        "18020-gamedata.base",    // Skeleton Repair Kit
        "45557-changes_otto.mod", // Skeleton Eye
        "43399-changes_otto.mod", // Press
        "43398-changes_otto.mod", // Power Core
        "42189-rebirth.mod",      // Generator Core
        "42318-changes_otto.mod", // Gears
        "43394-changes_otto.mod", // CPU Unit
        "43393-changes_otto.mod"  // Capacitor
    };

    if (gCtx.sellsRobotics &&
        stringInList(itemId, roboticsInputIds, sizeof(roboticsInputIds) / sizeof(roboticsInputIds[0])))
        return true;
    if (gCtx.isBar &&
        (cat == CAT_FOOD || cat == CAT_BOOZE || cat == CAT_WATER))
        return true;
    // Blueprint sellers reference the produced item, not the blueprint object,
    // so the sold blueprint's ID never matches stockIds - accept by category.
    if (gCtx.sellsBlueprints && cat == CAT_BLUEPRINT)
        return true;
    // Armour quality comes from faction data the vendor refs don't expose, so a
    // per-shop quality floor isn't readable - accept the whole category instead.
    if (gCtx.sellsArmorOrClothing && cat == CAT_ARMOUR)
        return true;
    // Weapon shops buy any weapon whose quality tier is at or above the lowest
    // tier they stock. Item::quality and the manufacturer "model value" floor are
    // both on the 1..100 scale, so they compare directly. Unreadable quality
    // accepts, rather than refusing a legit weapon over a read miss.
    if (cfg::enableWeaponTierAcceptance && gCtx.sellsWeapons &&
        (cat == CAT_WEAPON || cat == CAT_RANGED) && readable(item, 8))
    {
        if (item->quality < 0.0f || item->quality >= (float)gCtx.minWeaponModelValue)
            return true;
    }
    // A shop that stocks books buys any book (symmetric to bars/food).
    if (cat == CAT_BOOK && gCtx.stockCats.count(CAT_BOOK) > 0)
        return true;
    if (itemId.empty()) return false;

    loadReferenceLists();

    if ((gCtx.isWeaponsOnlyShop || gCtx.stockCats.count(CAT_RANGED) > 0) &&
        setHasItemID(gCrossbowForceIds, itemId))
        return true;
    if (gCtx.sellsArmorOrClothing && setHasItemID(gArmorForceIds, itemId))
        return true;
    if (gCtx.isBar &&
        (setHasItemID(gFoodIds, itemId) || setHasItemID(gDrinkWaterIds, itemId)))
        return true;
    if (gCtx.isTradeShop &&
        (cat == CAT_TRADEGOODS || isGeneralTradeAllowID(itemId) || isLootOnlyTradeItem(itemId)))
        return true;
    return false;
}

// Robot limb shops are inconsistent in FCS naming, so infer them from stock
// categories first. `LIMB_REPLACEMENT` stock is classified as CAT_ROBOTICS.
static bool stockLooksLikeRobotics(const ShopContext& ctx)
{
    return ctx.stockCats.count(CAT_ROBOTICS) > 0 ||
        ctx.archetype == "robotics";
}

struct VendorStockList { const char* name; Cat cat; };
static const VendorStockList kVendorStockLists[] = {
    { "crossbows", CAT_RANGED }, { "ammo", CAT_AMMO }, { "ammunition", CAT_AMMO }, { "bolts", CAT_AMMO },
    { "weapons", CAT_WEAPON }, { "armour", CAT_ARMOUR }, { "armor", CAT_ARMOUR },
    { "containers", CAT_BACKPACK }, { "backpacks", CAT_BACKPACK }, { "blueprints", CAT_BLUEPRINT },
    { "maps", CAT_MAP }, { "map", CAT_MAP }, { "medical", CAT_MEDICAL }, { "robotics", CAT_ROBOTICS },
    { "limbs", CAT_ROBOTICS }, { "robot limbs", CAT_ROBOTICS }, { "robotic limbs", CAT_ROBOTICS },
    { "food", CAT_FOOD },
    { "building materials", CAT_BUILDMATS }, { "raw materials", CAT_RAWMATS },
    { "trade goods", CAT_TRADEGOODS }, { "items", CAT_OTHER },
};

static Cat gameDataToCat(GameData* data, Cat hint)
{
    if (hint != CAT_OTHER) return hint;
    if (!data) return CAT_OTHER;
    Cat typeCat = gameDataTypeToCat(data->type);
    if (typeCat != CAT_OTHER) return typeCat;

    Cat refCat = referenceCategoryForItemID(toLower(data->stringID));
    if (refCat != CAT_OTHER) return refCat;

    std::string name = toLower(data->name + " " + data->stringID);
    Cat c;
    if (sectionToCat(name, &c)) return c;
    return CAT_OTHER;
}

static bool gameDataTypeCanBeShopStock(itemType t)
{
    switch (t)
    {
    case ITEM:
    case WEAPON:
    case CROSSBOW:
    case ARMOUR:
    case CONTAINER:
    case BLUEPRINT:
    case ARTIFACTS:
    case MAP_ITEM:
    case LIMB_REPLACEMENT:
        return true;
    default:
        return false;
    }
}

static Cat vendorRefListHint(const std::string& listName)
{
    std::string lower = toLower(listName);
    for (size_t i = 0; i < sizeof(kVendorStockLists) / sizeof(kVendorStockLists[0]); ++i)
        if (lower == kVendorStockLists[i].name)
            return kVendorStockLists[i].cat;

    Cat c;
    if (sectionToCat(lower, &c)) return c;
    if (lower.find("med") != std::string::npos || lower.find("first aid") != std::string::npos) return CAT_MEDICAL;
    if (lower.find("backpack") != std::string::npos || lower.find("container") != std::string::npos) return CAT_BACKPACK;
    if (lower.find("blueprint") != std::string::npos) return CAT_BLUEPRINT;
    if (lower.find("map") != std::string::npos) return CAT_MAP;
    if (lower.find("book") != std::string::npos) return CAT_BOOK;
    if (lower.find("food") != std::string::npos) return CAT_FOOD;
    if (lower.find("robot") != std::string::npos) return CAT_ROBOTICS;
    return CAT_OTHER;
}

static void logVendorItem(GameData* vendor, const std::string& listName,
                          GameData* itemData, Cat cat, int refIndex)
{
    if (!cfg::debug || !cfg::logVendorListItems) return;
    static std::set<std::string> seen;
    if ((int)seen.size() >= cfg::maxVendorItemLogs) return;

    std::string key = normaliseName(vendor ? vendor->name : std::string()) + "|" +
        listName + "|" + normaliseName(itemData ? itemData->name : std::string());
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    char buf[768];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][vendor_item] vendor=%s/%p list=%s idx=%d item=%s type=%d cat=%s",
        vendor ? vendor->name.c_str() : "", vendor, listName.c_str(), refIndex,
        itemData ? itemData->name.c_str() : "", itemData ? (int)itemData->type : -1,
        catLabel(cat));
    DebugLog(buf);
}

static void addStockGameData(GameData* vendor, GameData* itemData, Cat hint,
                             const std::string& listName, int refIndex, ShopContext& ctx)
{
    if (!itemData) return;
    if (!gameDataTypeCanBeShopStock(itemData->type)) return;

    ++ctx.templateStockRefs;
    Cat c = gameDataToCat(itemData, hint);
    if (c == CAT_OTHER)
    {
        std::string combined = listName + " " + itemData->name + " " + itemData->stringID;
        Cat named;
        if (sectionToCat(toLower(combined), &named)) c = named;
    }
    if (c != CAT_OTHER) ctx.stockCats.insert(c);

    std::string id = toLower(itemData->stringID);
    if (!id.empty()) ctx.stockIds.insert(id);

    std::string nm = normaliseName(itemData->name);
    if (!nm.empty()) ctx.stockNames.insert(nm);
    ctx.haveStock = true;
    logVendorItem(vendor, listName, itemData, c, refIndex);
}

typedef boost::unordered::unordered_map<
    std::string,
    Ogre::vector<GameDataReference>::type,
    boost::hash<std::string>,
    std::equal_to<std::string>,
    Ogre::STLAllocator<
        std::pair<std::string const, Ogre::vector<GameDataReference>::type>,
        Ogre::GeneralAllocPolicy> > GameDataReferenceMap;

// Adds exact item IDs from a vendor list. FCS stores many useful goods as
// generic ITEM data, so curated lists are more stable than display names.
static void addReferenceListItemsToSet(GameData* vendor, std::set<std::string>& out)
{
    if (!vendor) return;
    for (GameDataReferenceMap::const_iterator it = vendor->objectReferences.begin();
         it != vendor->objectReferences.end(); ++it)
    {
        const Ogre::vector<GameDataReference>::type& refs = it->second;
        uint32_t scanned = refs.size() < cfg::maxStockScan ? (uint32_t)refs.size() : cfg::maxStockScan;
        for (uint32_t i = 0; i < scanned; ++i)
        {
            GameData* itemData = refs[i].ptr;
            if (!itemData || !gameDataTypeCanBeShopStock(itemData->type)) continue;
            std::string id = toLower(itemData->stringID);
            if (!id.empty()) out.insert(id);
        }
    }
}

static void loadReferenceList(GameDataContainer& gamedata, const char* id,
                              std::set<std::string>& out, const char* label)
{
    GameData* vendor = gamedata.getData(std::string(id), VENDOR_LIST);
    if (!vendor)
    {
        if (cfg::debug)
        {
            char miss[192];
            _snprintf_s(miss, sizeof(miss), _TRUNCATE,
                "[TRII][ref_list] vendor list not found label=%s id=%s", label, id);
            DebugLog(miss);
        }
        return;
    }

    addReferenceListItemsToSet(vendor, out);
    if (cfg::debug)
    {
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][ref_list] loaded label=%s id=%s items=%d",
            label, id, (int)out.size());
        DebugLog(buf);
    }
}

// Loads curated FCS reference lists once. These lists give generic ITEM records
// stable category/allowance meaning without matching localized display names.
static void loadReferenceLists()
{
    if (gReferenceListsLoaded) return;

    if (!ou)
    {
        if (cfg::debug) DebugLog("[TRII][ref_list] GameWorld unavailable; reference lists not loaded");
        return;
    }
    gReferenceListsLoaded = true;

    loadReferenceList(ou->gamedata, "11-Traders Refuse Irrelevant Items.mod",
        gGeneralTradeAllowIds, "force accept general store");
    loadReferenceList(ou->gamedata, "1013-gamedata.base",
        gGeneralTradeAllowIds, "all trade goods");
    loadReferenceList(ou->gamedata, "1386-gamedata.base",
        gGeneralTradeAllowIds, "trade goods");

    loadReferenceList(ou->gamedata, "43969-rebirth.mod",
        gFoodIds, "all food");
    loadReferenceList(ou->gamedata, "12-Traders Refuse Irrelevant Items.mod",
        gDrinkWaterIds, "drink and water");
    loadReferenceList(ou->gamedata, "13-Traders Refuse Irrelevant Items.mod",
        gBookIds, "books");
    loadReferenceList(ou->gamedata, "56360-rebirth.mod",
        gUsefulBuildingMatIds, "useful building mats");
    loadReferenceList(ou->gamedata, "14-Traders Refuse Irrelevant Items.mod",
        gCrossbowForceIds, "crossbow force");
    loadReferenceList(ou->gamedata, "15-Traders Refuse Irrelevant Items.mod",
        gArmorForceIds, "armor force");

    if (cfg::debug)
    {
        char buf[384];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][ref_list] totals trade=%d food=%d drinkWater=%d books=%d building=%d crossbow=%d armor=%d",
            (int)gGeneralTradeAllowIds.size(), (int)gFoodIds.size(),
            (int)gDrinkWaterIds.size(), (int)gBookIds.size(),
            (int)gUsefulBuildingMatIds.size(), (int)gCrossbowForceIds.size(),
            (int)gArmorForceIds.size());
        DebugLog(buf);
    }
}

static bool isGeneralTradeAllowID(const std::string& itemId)
{
    loadReferenceLists();
    return gGeneralTradeAllowIds.find(itemId) != gGeneralTradeAllowIds.end();
}

static bool setHasItemID(const std::set<std::string>& ids, const std::string& itemId)
{
    return !itemId.empty() && ids.find(itemId) != ids.end();
}

static Cat referenceCategoryForItemID(const std::string& itemId)
{
    if (itemId.empty()) return CAT_OTHER;
    loadReferenceLists();
    if (setHasItemID(gFoodIds, itemId)) return CAT_FOOD;
    if (setHasItemID(gDrinkWaterIds, itemId)) return CAT_BOOZE;
    if (setHasItemID(gBookIds, itemId)) return CAT_BOOK;
    if (setHasItemID(gUsefulBuildingMatIds, itemId)) return CAT_BUILDMATS;
    return CAT_OTHER;
}

// Scan the shop's live inventory into a category + stock-ID profile.
static void buildStockProfile(Inventory* inv, ShopContext& ctx)
{
    if (!inv) return;
    const lektor<Item*>& items = inv->getAllItems();
    uint32_t n = items.size();
    if (n == 0) return;
    ctx.liveStockItems += (int)n;
    uint32_t scanned = n < cfg::maxStockScan ? n : cfg::maxStockScan;
    for (uint32_t i = 0; i < scanned; ++i)
    {
        Item* it = items[i];
        if (!it) continue;
        ctx.stockCats.insert(classifyItem(it));
        std::string id = itemStringID(it);
        if (!id.empty()) ctx.stockIds.insert(id);
        std::string nm = normaliseName(safeName(it));
        if (!nm.empty()) ctx.stockNames.insert(nm);
    }
    ctx.haveStock = !ctx.stockCats.empty();
}

// FCS blueprint vendor lists (keys like "armour blueprints", "crossbow
// blueprints", "BLUEPRINT_ITEM_ARMOUR") reference the *produced* item data
// (ARMOUR/WEAPON), never the BLUEPRINT-typed object the player later sells, so
// those blueprint stringIDs never enter stockIds. Flagging the shop as a
// blueprint seller off the list KEY lets us accept blueprint sales by category.
static bool keyLooksLikeBlueprintList(const std::string& key)
{
    return toLower(key).find("blueprint") != std::string::npos;
}

// A WEAPON_MANUFACTURER holds a "weapon models" reference list; each row's first
// TripleInt value is a model's "model value" tier (1..100). The shop's lowest
// such value is its tier floor - a sold weapon's Item::quality (same 1..100
// scale) is later compared against it.
static void scanWeaponManufacturer(GameData* manufacturer, ShopContext& ctx)
{
    if (!manufacturer || manufacturer->type != WEAPON_MANUFACTURER) return;
    for (GameDataReferenceMap::const_iterator it = manufacturer->objectReferences.begin();
         it != manufacturer->objectReferences.end(); ++it)
    {
        if (toLower(it->first).find("model") == std::string::npos) continue;
        const Ogre::vector<GameDataReference>::type& refs = it->second;
        uint32_t scanned = refs.size() < cfg::maxStockScan ? (uint32_t)refs.size() : cfg::maxStockScan;
        for (uint32_t j = 0; j < scanned; ++j)
        {
            int value = refs[j].values.value[0];
            if (value <= 0 || value > 100) continue;
            if (!ctx.hasWeaponModelTier || value < ctx.minWeaponModelValue)
            {
                ctx.minWeaponModelValue = value;
                ctx.hasWeaponModelTier = true;
            }
        }
    }
}

static void addVendorTemplateRefs(GameData* vendor, ShopContext& ctx)
{
    if (!vendor) return;
    ctx.foundVendorList = true;
    if (isGeneralTradeAllowList(vendor))
        ctx.hasGeneralTradeAllowList = true;
    if (ctx.seenVendorLists.count(vendor)) return;
    ctx.seenVendorLists.insert(vendor);
    if (!vendor->name.empty()) ctx.vendorListNames.insert(vendor->name);

    if (cfg::scanAllVendorRefs)
    {
        bool generalTradeList = isGeneralTradeAllowList(vendor);
        for (GameDataReferenceMap::const_iterator it = vendor->objectReferences.begin();
             it != vendor->objectReferences.end(); ++it)
        {
            if (keyLooksLikeBlueprintList(it->first)) ctx.sellsBlueprints = true;
            Cat hint = referenceListCategoryHint(vendor);
            if (hint == CAT_OTHER)
                hint = vendorRefListHint(it->first);
            if (generalTradeList && hint == CAT_OTHER)
                hint = CAT_TRADEGOODS;
            const Ogre::vector<GameDataReference>::type& refs = it->second;
            uint32_t scanned = refs.size() < cfg::maxStockScan ? (uint32_t)refs.size() : cfg::maxStockScan;
            for (uint32_t j = 0; j < scanned; ++j)
            {
                // Weapon manufacturers carry the model-value tiers, not sellable
                // stock, so they never pass gameDataTypeCanBeShopStock - handle
                // them here before addStockGameData drops them.
                if (cfg::enableWeaponTierAcceptance && refs[j].ptr &&
                    refs[j].ptr->type == WEAPON_MANUFACTURER)
                    scanWeaponManufacturer(refs[j].ptr, ctx);
                addStockGameData(vendor, refs[j].ptr, hint, it->first, (int)j, ctx);
            }
        }
    }
    else
    {
        for (size_t i = 0; i < sizeof(kVendorStockLists) / sizeof(kVendorStockLists[0]); ++i)
        {
            const Ogre::vector<GameDataReference>::type* refs =
                vendor->getReferenceListIfExists(kVendorStockLists[i].name);
            if (!refs || refs->empty()) continue;
            if (keyLooksLikeBlueprintList(kVendorStockLists[i].name)) ctx.sellsBlueprints = true;
            Cat hint = kVendorStockLists[i].cat;
            if (ctx.hasGeneralTradeAllowList && hint == CAT_OTHER)
                hint = CAT_TRADEGOODS;
            if (hint != CAT_OTHER) ctx.stockCats.insert(hint);
            uint32_t scanned = refs->size() < cfg::maxStockScan ? (uint32_t)refs->size() : cfg::maxStockScan;
            for (uint32_t j = 0; j < scanned; ++j)
                addStockGameData(vendor, (*refs)[j].ptr, hint, kVendorStockLists[i].name, (int)j, ctx);
        }
    }
}

static void logVendorListFound(GameData* ownerData, const std::string& memberName,
                               GameData* vendor, int index)
{
    if (!cfg::debug || !cfg::logVendorListItems || !vendor) return;
    static std::set<std::string> seen;
    std::string key = normaliseName(ownerData ? ownerData->name : std::string()) + "|" +
        memberName + "|" + normaliseName(vendor->name);
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    char buf[640];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][vendor_list] owner=%s ownerType=%d member=%s idx=%d vendor=%s/%p refs=%d",
        ownerData ? ownerData->name.c_str() : "", ownerData ? (int)ownerData->type : -1,
        memberName.c_str(), index, vendor->name.c_str(), vendor,
        (int)vendor->objectReferences.size());
    DebugLog(buf);
}

// Folds a gamedata's vendor stock into the context: if it is itself a vendor
// list, scan it directly; otherwise scan the vendor lists it points at.
static void addVendorListsReferencedBy(GameData* data, ShopContext& ctx)
{
    if (!data) return;
    if (data->type == VENDOR_LIST)
    {
        addVendorTemplateRefs(data, ctx);
        return;
    }

    for (GameDataReferenceMap::const_iterator it = data->objectReferences.begin();
         it != data->objectReferences.end(); ++it)
    {
        const Ogre::vector<GameDataReference>::type& refs = it->second;
        uint32_t scanned = refs.size() < 32 ? (uint32_t)refs.size() : 32;
        for (uint32_t i = 0; i < scanned; ++i)
        {
            GameData* ref = refs[i].ptr;
            if (ref && ref->type == VENDOR_LIST)
            {
                logVendorListFound(data, it->first, ref, (int)i);
                addVendorTemplateRefs(ref, ctx);
            }
        }
    }
}

static void buildTemplateStockProfile(GameData* squadTemplate, ShopContext& ctx)
{
    addVendorListsReferencedBy(squadTemplate, ctx);
}

static void buildTemplateStockProfile(Character* shop, ShopContext& ctx)
{
    if (!shop) return;
    addVendorListsReferencedBy(shop->getGameData(), ctx);

    ActivePlatoon* active = shop->getPlatoon();
    if (active && readable(active->me, 8))
        buildTemplateStockProfile(active->me->squadTemplate, ctx);

    const hand& indoors = shop->isIndoors();
    if (indoors)
    {
        RootObjectBase* building = indoors.getRootObjectBase();
        if (building && readable(building, 8))
            addVendorListsReferencedBy(building->getGameData(), ctx);
    }
}

static void logNoVendorListFound(RootObject* shop)
{
    if (!cfg::debug) return;
    static std::set<std::string> seen;
    std::string name = readable(shop, 8) ? safeName(shop) : std::string();
    std::string key = normaliseName(name);
    if (key.empty())
    {
        char ptrKey[64];
        _snprintf_s(ptrKey, sizeof(ptrKey), _TRUNCATE, "%p", shop);
        key = ptrKey;
    }
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    char buf[384];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][shop] No vendor list found for %s", name.c_str());
    DebugLog(buf);
}

// FCS squad-definition stringID (e.g. "56154-rebirth.mod") for the shop's placed
// squad, lowercased; empty when unavailable. Pins trader behaviour to a specific
// placed squad rather than a display name that can collide across the world.
static std::string shopSquadTemplateID(RootObject* shop)
{
    if (!isCharacterType(shop)) return std::string();
    ActivePlatoon* active = ((Character*)shop)->getPlatoon();
    if (!readable(active, 8) || !readable(active->me, 8)) return std::string();
    GameData* squad = active->me->squadTemplate;
    if (!readable(squad, 8)) return std::string();
    return toLower(squad->stringID);
}

// Squads whose trader accepts every item regardless of stock (leader trades as a
// fence). Keyed on the FCS squad stringID so it targets exactly the placed squad.
static bool squadIsAcceptAll(const std::string& squadId)
{
    if (squadId.empty()) return false;
    static const char* kAcceptAllSquads[] = {
        "56154-rebirth.mod",   // Quin - Scraphouse squad, Shem (accepts everything)
    };
    for (size_t i = 0; i < sizeof(kAcceptAllSquads) / sizeof(kAcceptAllSquads[0]); ++i)
        if (squadId == toLower(kAcceptAllSquads[i])) return true;
    return false;
}

// Fencing traders are the Shinobi Thieves' fences. Their placed characters carry
// a range of display names (Thief Fence, Shinobi Trader, ...), so back the name
// hints with a faction check: any shopkeeper whose faction is the Shinobi Thieves
// trades as a fence. Keyed on the faction name/stringID (vanilla "17-gamedata.base").
static bool shopFactionIsThievesGuild(RootObject* shop)
{
    if (!isCharacterType(shop)) return false;
    Ownerships* own = ((Character*)shop)->getOwnerships();
    if (!readable(own, 8)) return false;
    Faction* faction = own->faction;
    if (!readable(faction, 8)) return false;

    std::string key = toLower(faction->getName());
    GameData* data = faction->getData();
    if (readable(data, 8))
        key += " " + toLower(data->stringID);

    return key.find("shinobi thieves") != std::string::npos ||
        key.find("17-gamedata.base") != std::string::npos;
}

static void deriveShopTraits(ShopContext& ctx)
{
    std::string shopName = readable(ctx.shop, 8) ? normaliseName(safeName(ctx.shop)) : std::string();
    ctx.isThiefFence = shopName == "thief fence" || shopFactionIsThievesGuild(ctx.shop);
    ctx.isAcceptAllVendor = ctx.isThiefFence || shopName == "shinobi trader" ||
        squadIsAcceptAll(shopSquadTemplateID(ctx.shop));
    ctx.isSkeletonVendor = isCharacterType(ctx.shop) && characterIsSkeleton((Character*)ctx.shop);
    ctx.sellsFood = ctx.stockCats.count(CAT_FOOD) > 0;
    ctx.sellsRobotics = stockLooksLikeRobotics(ctx);
    ctx.isWeaponsOnlyShop = stockHasOnlyWeaponGoods(ctx.stockCats);
    ctx.sellsWeapons = ctx.stockCats.count(CAT_WEAPON) > 0 || ctx.stockCats.count(CAT_RANGED) > 0 ||
        ctx.archetype == "weaponsmith";
    ctx.sellsArmorOrClothing = ctx.stockCats.count(CAT_ARMOUR) > 0 || ctx.archetype == "armoursmith";
    ctx.isBar = ctx.archetype == "bar" || ctx.stockCats.count(CAT_BOOZE) > 0;
    ctx.isTradeShop = ctx.hasGeneralTradeAllowList ||
        ctx.stockCats.count(CAT_TRADEGOODS) > 0 ||
        ctx.archetype == "general_store" ||
        ctx.archetype == "wandering_trader";
}

// Recompute the cached context when the trader pointer changes.
static void ensureContext(RootObject* shop, Inventory* stockInv)
{
    if (shop == gCtx.shop) return;
    gCtx.clear();
    gCtx.shop = shop;
    if (!shop) return;
    gCtx.archetype = classifyShop(shop);
    if (cfg::useStockAcceptance)
    {
        if (isCharacterType(shop))
            buildTemplateStockProfile((Character*)shop, gCtx);
        if (!gCtx.foundVendorList && cfg::acceptAllWhenNoVendorList)
            logNoVendorListFound(shop);
        else if (gCtx.templateStockRefs <= 0)
            buildStockProfile(stockInv, gCtx);
        deriveShopTraits(gCtx);
    }
    if (cfg::debug)
    {
        std::string cats = stockCatSummary(gCtx.stockCats);
        std::string vendorLists = vendorListSummary(gCtx.vendorListNames);
        std::string stockNames = stockNameSummary(gCtx.stockNames);
        char buf[1280];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][shop] name=%s archetype=%s foundVendorList=%d haveStock=%d stockCats=%d stockCatList=%s stockIds=%d stockNames=%d stockNameSample=%s vendorLists=%d vendorListNames=%s tradeAllowList=%d tradeShop=%d robotics=%d weaponOnly=%d sellsWeapons=%d armour=%d bar=%d thiefFence=%d acceptAll=%d skeletonVendor=%d sellsFood=%d sellsBlueprints=%d weaponTier=%d minModelValue=%d liveItems=%d templateRefs=%d",
            safeName(shop).c_str(), gCtx.archetype.c_str(), (int)gCtx.foundVendorList, (int)gCtx.haveStock,
            (int)gCtx.stockCats.size(), cats.c_str(), (int)gCtx.stockIds.size(), (int)gCtx.stockNames.size(),
            stockNames.c_str(), (int)gCtx.vendorListNames.size(), vendorLists.c_str(),
            (int)gCtx.hasGeneralTradeAllowList, (int)gCtx.isTradeShop, (int)gCtx.sellsRobotics,
            (int)gCtx.isWeaponsOnlyShop, (int)gCtx.sellsWeapons, (int)gCtx.sellsArmorOrClothing, (int)gCtx.isBar,
            (int)gCtx.isThiefFence, (int)gCtx.isAcceptAllVendor, (int)gCtx.isSkeletonVendor,
            (int)gCtx.sellsFood, (int)gCtx.sellsBlueprints, (int)gCtx.hasWeaponModelTier,
            gCtx.minWeaponModelValue, gCtx.liveStockItems, gCtx.templateStockRefs);
        DebugLog(buf);
    }
}

// ---------------------------------------------------------------------
//  DISPOSITION
// ---------------------------------------------------------------------
enum Disp { DISP_FULL, DISP_REDUCED, DISP_REFUSE };

static const char* dispLabel(Disp disp)
{
    switch (disp)
    {
    case DISP_FULL: return "full";
    case DISP_REDUCED: return "reduced";
    default: return "refuse";
    }
}

static Disp baseDisposition(const Rule& rule, Cat cat, double* reducedMult)
{
    if (rule.full.count(cat)) return DISP_FULL;
    std::map<Cat, double>::const_iterator it = rule.reduced.find(cat);
    if (it != rule.reduced.end()) { if (reducedMult) *reducedMult = it->second; return DISP_REDUCED; }
    if (cat == CAT_OTHER && !cfg::refuseUnknown) return DISP_FULL;
    return DISP_REFUSE;
}

static bool itemIsFoodForSale(Cat cat, const std::string& itemId)
{
    if (cat == CAT_FOOD) return true;
    return referenceCategoryForItemID(itemId) == CAT_FOOD;
}

// The shop's real stock expands acceptance; otherwise fall back to the rule.
// `item` (optional) supplies the weapon manufacturer-tier check.
static Disp dispositionFor(Cat cat, const std::string& itemId, double* reducedMult,
                           InventoryItemBase* item = NULL)
{
    // Accept-all vendors (fences, and squads pinned via squadIsAcceptAll like Quin)
    // buy everything at full price - checked before the skeleton food guard so a
    // skeleton accept-all trader still takes food.
    if (gCtx.isAcceptAllVendor) return DISP_FULL;
    if (gCtx.isSkeletonVendor && !gCtx.sellsFood && itemIsFoodForSale(cat, itemId))
        return DISP_REFUSE;

    const Rule& rule = ruleFor(gCtx.archetype);
    if (cfg::useStockAcceptance && gCtx.haveStock)
    {
        if (!itemId.empty() && gCtx.stockIds.count(itemId)) return DISP_FULL;
        if (isShopSpecificAcceptedGood(itemId, cat, item)) return DISP_FULL;
        if (cfg::strictVendorStockAcceptance)
        {
            if (itemIsFoodForSale(cat, itemId))
            {
                if (reducedMult) *reducedMult = cfg::offListFoodMult;
                return DISP_REDUCED;
            }
            return DISP_REFUSE;
        }
        if (cat != CAT_OTHER && gCtx.stockCats.count(cat)) return DISP_FULL;
        if (cat == CAT_FOOD)
        {
            if (reducedMult) *reducedMult = cfg::offListFoodMult;
            return DISP_REDUCED;
        }
        // stock is known and the item matched neither category nor ID:
        // still honour a reduced rule (e.g. food), else refuse.
        std::map<Cat, double>::const_iterator it = rule.reduced.find(cat);
        if (it != rule.reduced.end()) { if (reducedMult) *reducedMult = it->second; return DISP_REDUCED; }
        // With a vendor list present, unknown/off-list items should not slip
        // through just because the classifier could not name their category.
        return DISP_REFUSE;
    }
    if (cfg::useStockAcceptance && cfg::strictVendorStockAcceptance && gCtx.foundVendorList)
        return DISP_REFUSE;
    if (cfg::useStockAcceptance && cfg::acceptAllWhenNoVendorList && !gCtx.foundVendorList)
        return DISP_FULL;
    return baseDisposition(rule, cat, reducedMult);
}

// ---------------------------------------------------------------------
//  FAULT DIAGNOSTICS
//  SEH turns a bad dereference into RE'd game memory into a logged reason +
//  safe default, instead of crashing Kenshi. This filter records the fault
//  site, exception code and address ONCE per site (so it can't spam a
//  per-frame hook), then runs the __except body (which returns the default).
//  Caveat: C++ locals in the faulting *_impl frame are not unwound on this path
//  (SEH runs no C++ destructors) - a bounded leak, since each site logs once.
// ---------------------------------------------------------------------
static std::set<std::string> gSeenFaults;

static LONG triiSehFilter(const char* where, EXCEPTION_POINTERS* ep)
{
    std::string key(where);
    if (gSeenFaults.find(key) == gSeenFaults.end())
    {
        gSeenFaults.insert(key);
        unsigned long code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
        void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : NULL;
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "TradersRefuse: FAULT in %s (code=0x%08lX addr=%p) - failing open",
            where, code, addr);
        ErrorLog(buf);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// One-shot breadcrumb: logs `msg` the first time `flag` is seen false, so a
// per-frame hook drops exactly one "I was entered / reached here" marker. The
// last breadcrumb before a crash line localises the fault even when SEH can't
// catch it (e.g. a crash inside a called engine function on another frame).
static void onceLog(bool& flag, const char* msg)
{
    // ErrorLog (not DebugLog) so the marker is flushed to disk immediately -
    // critical for capturing the LAST breadcrumb before a hard crash.
    if (!flag) { flag = true; ErrorLog(msg); }
}

// Log a non-fault reason we backed off (unexpected null / bad pointer), once
// per distinct message, so the debug log explains a no-op without a crash.
static void triiNote(const char* msg)
{
    std::string key(msg);
    if (gSeenFaults.find(key) != gSeenFaults.end()) return;
    gSeenFaults.insert(key);
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "TradersRefuse: %s", msg);
    ErrorLog(buf);
}

// Is [p, p+n) committed and readable? Cheap gate for externally-sourced
// pointers so an obvious dud logs a clear reason instead of faulting.
static bool readable(const void* p, size_t n)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD readOk = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readOk)) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return ((uintptr_t)p + n) <= regionEnd;
}

static GameData* itemGameData(InventoryItemBase* item)
{
    if (!readable(item, 8)) return NULL;
    GameData* data = item->getGameData();
    if (!readable(data, 8)) return NULL;
    return data;
}

static std::string itemStringID(InventoryItemBase* item)
{
    GameData* data = itemGameData(item);
    if (!data) return std::string();
    return toLower(data->stringID);
}

// ---------------------------------------------------------------------
//  HOOK 1: value cue on the player's sell side
//  InventoryItemBase::getValueSingle(bool isPlayer) is virtual, so hook the
//  non-virtual body via &InventoryItemBase::_NV_getValueSingle. Subclasses may
//  keep their own value path, so hard refusal must still live in add/placement.
// ---------------------------------------------------------------------
typedef int (*GetValueSingle_t)(InventoryItemBase*, bool);
static GetValueSingle_t GetValueSingle_orig = NULL;

// getValueAll (RVA 0x790350) prices a whole stack and is a SEPARATE engine
// function from getValueSingle - so a refused/repriced stack sold via the stack
// path would otherwise be valued (and paid) at full price, bypassing repricing.
typedef int (*GetValueAll_t)(InventoryItemBase*, bool);
static GetValueAll_t GetValueAll_orig = NULL;

static bool gDbgValueFire = false;
static bool gDbgValueAllFire = false;
static bool gDbgValueSell = false;
static std::set<std::string> gSeenValueProfiles;

static float safeTraderPriceMultiplier()
{
    float mult = 1.0f;
    __try
    {
        mult = InventoryGUI::getTraderPriceMultiplier();
    }
    __except (triiSehFilter("value.traderMultiplier", GetExceptionInformation()))
    {
        return 1.0f;
    }
    if (mult < 0.01f || mult > 10.0f) return 1.0f;
    return mult;
}

// InventoryItemBase::merchantPriceMod() is the item's *local* (regional) price
// modifier - the same town-economy factor the engine folds into a real buy/sell
// price. It is protected, so reach it through a derived-cast accessor (the
// IGUIHookAccess pattern); we never instantiate this type.
struct ItemPriceAccess : public InventoryItemBase
{
    static float merchantMod(InventoryItemBase* item)
    {
        return static_cast<ItemPriceAccess*>(item)->merchantPriceMod();
    }
};

// getAvgPrice() is the *global* base value with no regional adjustment. Paying
// that as the "full local price" overpays in cheap regions (and underpays in
// expensive ones): the player could buy grog low and sell it back higher for
// free money. Fold the regional modifier back in so the value we pay tracks the
// same local price the shop bought at.
static float safeMerchantPriceMod(InventoryItemBase* item)
{
    if (!readable(item, 8)) return 1.0f;
    float mod = 1.0f;
    __try
    {
        mod = ItemPriceAccess::merchantMod(item);
    }
    __except (triiSehFilter("value.merchantMod", GetExceptionInformation()))
    {
        return 1.0f;
    }
    if (mod < 0.01f || mod > 100.0f) return 1.0f;
    return mod;
}

// Pre-divide by the trader's global price multiplier so the engine's own later
// multiply lands back on the local price we actually want to pay.
static int cancelTraderSellMultiplier(int desiredValue, float traderMult)
{
    if (!cfg::payFullLocalPrice) return desiredValue;
    if (desiredValue <= 0) return desiredValue;
    if (traderMult < 0.01f || traderMult > 10.0f) return desiredValue;
    if (std::fabs(traderMult - 1.0f) < 0.001f) return desiredValue;
    return (int)std::ceil((double)desiredValue / (double)traderMult);
}

static void logValueDecision(InventoryItemBase* item, Cat cat, Disp disp,
                             int base, int avg, int returned, float traderMult,
                             int buyValue = -1)
{
    if (!cfg::debug) return;
    if (gSeenValueProfiles.size() >= 80) return;
    std::string section = item ? item->inventorySection : std::string();
    std::string itemName = item ? safeName((RootObject*)item) : std::string();
    std::string itemId = itemStringID(item);
    std::string key = gCtx.archetype + "|" + catLabel(cat) + "|" +
        dispLabel(disp) + "|" + section + "|" + itemId + "|" + itemName;
    if (gSeenValueProfiles.find(key) != gSeenValueProfiles.end()) return;
    gSeenValueProfiles.insert(key);

    // Diagnostic for weapon-tier calibration: item quality + manufacturer vs the
    // shop's resolved model-value floor. Lets us see which scale `quality` uses.
    float quality = readable(item, 8) ? item->quality : -1.0f;
    GameData* manuf = readable(item, 8) ? item->manufacturerData : NULL;
    std::string manufId = readable(manuf, 8) ? manuf->stringID : std::string();
    std::string manufName = readable(manuf, 8) ? manuf->name : std::string();

    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][item] shop=%s archetype=%s item=%s itemId=%s section=%s fn=%d cat=%s disp=%s base=%d avg=%d buyValue=%d ret=%d traderMult=%.3f quality=%.3f manuf=%s/%s minModelValue=%d stockCats=%d stockIds=%d stockNames=%d",
        safeName(gCtx.shop).c_str(), gCtx.archetype.c_str(), itemName.c_str(), itemId.c_str(), section.c_str(),
        item ? (int)item->itemFunction : -1, catLabel(cat), dispLabel(disp),
        base, avg, buyValue, returned, traderMult, quality, manufName.c_str(), manufId.c_str(),
        gCtx.minWeaponModelValue, (int)gCtx.stockCats.size(),
        (int)gCtx.stockIds.size(), (int)gCtx.stockNames.size());
    DebugLog(buf);
}

// All the C++ logic (std::string/std::set locals) lives here so the SEH shell
// below stays free of objects that need unwinding (MSVC C2712).
//
// Shared repricing decision: returns the repriced *single-item* value and, via
// dispOut, the disposition. Both the getValueSingle and getValueAll hooks call
// this so a stack sale is priced/refused identically to a single sale.
static int repriceSingleValue(InventoryItemBase* self, bool isPlayer, int base, Disp* dispOut)
{
    if (dispOut) *dispOut = DISP_FULL;
    if (!cfg::enabled || !cfg::enableValueHook) return base;
    if (isPlayer != cfg::playerSellValue) return base;  // sell side only - cheap gate
                                                        // FIRST so non-sell/world
                                                        // valuations skip the rest
                                                        // (incl. the VirtualQuery)
    if (!readable(self, 8)) { triiNote("value: unreadable item ptr"); return base; }

    // Resolve the live trader; null unless a trade is actually open.
    Character* shop = InventoryGUI::getNPCTrader();
    if (!shop) return base;
    if (!readable(shop, 8)) { triiNote("value: unreadable trader ptr"); return base; }
    onceLog(gDbgValueSell, "TradersRefuse: [bc] value hook sell-side valuation (trader non-null)");

    Inventory* stockInv = ((RootObject*)shop)->getInventory();
    ensureContext((RootObject*)shop, stockInv);

    Cat cat = classifyByFunctionAndSection(self);
    double mult = 1.0;
    Disp disp = dispositionFor(cat, itemStringID(self), &mult, self);
    if (dispOut) *dispOut = disp;
    float traderMult = safeTraderPriceMultiplier();

    if (gCtx.isAcceptAllVendor && disp == DISP_FULL)
    {
        logValueDecision(self, cat, disp, base, -1, base, traderMult);
        return base;
    }

    if (disp == DISP_REFUSE)
    {
        logValueDecision(self, cat, disp, base, -1, cfg::refusedPreviewValue, traderMult);
        return cfg::refusedPreviewValue;
    }
    if (cfg::observeOnly)
    {
        logValueDecision(self, cat, disp, base, -1, base, traderMult);
        return base;
    }
    float localMod = safeMerchantPriceMod(self);
    // The player can always re-buy at the shop's buy price, so paying more than
    // that to buy the item back is a money pump (buy 925, our sell 1155 -> +230).
    // getValueSingle(isPlayer=false) is the buy-side value in the same units we
    // return, and the engine display-transforms both sides identically, so
    // clamping our return to it guarantees sell <= buy regardless of multipliers.
    int buyValue = GetValueSingle_orig ? GetValueSingle_orig(self, false) : 0;
    if (disp == DISP_REDUCED)
    {
        int avg = self->getAvgPrice();
        int desired = (int)std::floor(avg * localMod * mult);
        int ret = cancelTraderSellMultiplier(desired, traderMult);
        if (buyValue > 0 && ret > buyValue) ret = buyValue;
        logValueDecision(self, cat, disp, base, avg, ret, traderMult, buyValue);
        return ret;
    }
    if (disp == DISP_FULL && cfg::payFullLocalPrice)
    {
        int avg = self->getAvgPrice();
        int desired = (int)std::floor(avg * localMod);
        int ret = cancelTraderSellMultiplier(desired, traderMult);
        if (buyValue > 0 && ret > buyValue) ret = buyValue;
        logValueDecision(self, cat, disp, base, avg, ret, traderMult, buyValue);
        return ret;
    }
    logValueDecision(self, cat, disp, base, -1, base, traderMult);
    return base;
}

static int GetValueSingle_impl(InventoryItemBase* self, bool isPlayer, int base)
{
    return repriceSingleValue(self, isPlayer, base, NULL);
}

// Stack valuation. Reprice per item via the shared decision, then scale by
// quantity so N items in a stack are priced exactly like N single sales. A
// refused stack collapses to the flat refused sentinel (0 while testing) so the
// engine pays nothing for goods the shop won't take, even down the stack path.
static int GetValueAll_impl(InventoryItemBase* self, bool isPlayer, int base)
{
    if (!cfg::enabled || !cfg::enableValueHook) return base;
    if (isPlayer != cfg::playerSellValue) return base;
    if (!readable(self, 8)) { triiNote("valueAll: unreadable item ptr"); return base; }

    Character* shop = InventoryGUI::getNPCTrader();
    if (!shop) return base;
    if (!readable(shop, 8)) { triiNote("valueAll: unreadable trader ptr"); return base; }

    // The true per-item base comes from the original single valuation (bypassing
    // our own hook), so repricing lands on the same number the single path uses.
    int singleBase = GetValueSingle_orig ? GetValueSingle_orig(self, isPlayer) : 0;
    Disp disp = DISP_FULL;
    int perItem = repriceSingleValue(self, isPlayer, singleBase, &disp);

    if (disp == DISP_REFUSE) return cfg::refusedPreviewValue;
    if (cfg::observeOnly) return base;

    int qty = self->quantity > 0 ? self->quantity : 1;
    return perItem * qty;
}

static int GetValueSingle_hook(InventoryItemBase* self, bool isPlayer)
{
    onceLog(gDbgValueFire, "TradersRefuse: [bc] value hook first fire");

    // Stage 1: the original valuation. If this faults the hook plumbing itself
    // is wrong (distinct log), and we can't recover a base value.
    int base = 0;
    __try
    {
        base = GetValueSingle_orig(self, isPlayer);
    }
    __except (triiSehFilter("value.orig (hook plumbing)", GetExceptionInformation()))
    {
        return 0;
    }

    // Stage 2: our repricing logic. A fault here is our bug - fail open to base.
    __try
    {
        return GetValueSingle_impl(self, isPlayer, base);
    }
    __except (triiSehFilter("value.logic", GetExceptionInformation()))
    {
        return base;
    }
}

static int GetValueAll_hook(InventoryItemBase* self, bool isPlayer)
{
    onceLog(gDbgValueAllFire, "TradersRefuse: [bc] valueAll hook first fire");

    int base = 0;
    __try
    {
        base = GetValueAll_orig(self, isPlayer);
    }
    __except (triiSehFilter("valueAll.orig (hook plumbing)", GetExceptionInformation()))
    {
        return 0;
    }

    __try
    {
        return GetValueAll_impl(self, isPlayer, base);
    }
    __except (triiSehFilter("valueAll.logic", GetExceptionInformation()))
    {
        return base;
    }
}

// ---------------------------------------------------------------------
//  Protected InventoryGUI access
//  The derived wrapper reaches protected helpers used by the working
//  mouse-placement guard.
// ---------------------------------------------------------------------
typedef bool (*PlaceItemFromMouse_t)(
    InventoryGUI*, const std::string&, const MyGUI::types::TPoint<int>&);

static PlaceItemFromMouse_t PlaceItemFromMouse_orig = NULL;

struct IGUIHookAccess : public InventoryGUI
{
    static intptr_t placeItemFromMouseAddr() { return KenshiLib::GetRealAddress(&IGUIHookAccess::placeItemFromMouse); }
    static Item* mouseItemFor(InventoryGUI* gui)
    {
        if (!gui) return NULL;
        return static_cast<IGUIHookAccess*>(gui)->getMouseItem();
    }
};

static RootObject* inventoryOwner(Inventory* inv)
{
    if (!readable(inv, 8)) return NULL;
    RootObject* obj = inv->getCallbackObject();
    if (readable(obj, 8)) return obj;
    obj = inv->getOwner();
    if (readable(obj, 8)) return obj;
    return NULL;
}

static bool sameTradeObject(RootObject* a, RootObject* b)
{
    if (!readable(a, 8) || !readable(b, 8)) return false;
    if (a == b) return true;

    std::string an = normaliseName(safeName(a));
    std::string bn = normaliseName(safeName(b));
    return !an.empty() && an == bn;
}

static const char* dataTypeLabel(itemType t)
{
    switch (t)
    {
    case CHARACTER: return "character";
    case HUMAN_CHARACTER: return "human";
    case ANIMAL_CHARACTER: return "animal";
    case SHOP_TRADER_CLASS: return "shop_trader";
    case PLATOON: return "platoon";
    case WEAPON: return "weapon_data";
    case CROSSBOW: return "crossbow_data";
    case ARMOUR: return "armour_data";
    case ITEM: return "item_data";
    case VENDOR_LIST: return "vendor_list";
    default: return "other_type";
    }
}

static const char* attachSlotLabel(AttachSlot slot)
{
    switch (slot)
    {
    case ATTACH_WEAPON: return "weapon";
    case ATTACH_BACK: return "back";
    case ATTACH_HAIR: return "hair";
    case ATTACH_HAT: return "hat";
    case ATTACH_EYES: return "eyes";
    case ATTACH_BODY: return "body";
    case ATTACH_LEGS: return "legs";
    case ATTACH_NONE: return "none";
    case ATTACH_SHIRT: return "shirt";
    case ATTACH_BOOTS: return "boots";
    case ATTACH_GLOVES: return "gloves";
    case ATTACH_NECK: return "neck";
    case ATTACH_BACKPACK: return "backpack";
    case ATTACH_BEARD: return "beard";
    case ATTACH_BELT: return "belt";
    case ATTACH_LEFT_ARM: return "left_arm";
    case ATTACH_RIGHT_ARM: return "right_arm";
    case ATTACH_LEFT_LEG: return "left_leg";
    case ATTACH_RIGHT_LEG: return "right_leg";
    default: return "unknown";
    }
}

static std::string objectSummary(RootObject* obj)
{
    if (!readable(obj, 8)) return "null";
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s/%s/%p",
        safeName(obj).c_str(), dataTypeLabel(obj->getDataType()), obj);
    return std::string(buf);
}

enum RaceGroup
{
    RACE_UNKNOWN, RACE_HUMAN, RACE_SHEK, RACE_HIVER, RACE_SKELETON, RACE_ANIMAL
};

static const char* raceGroupLabel(RaceGroup race)
{
    switch (race)
    {
    case RACE_HUMAN: return "human";
    case RACE_SHEK: return "shek";
    case RACE_HIVER: return "hiver";
    case RACE_SKELETON: return "skeleton";
    case RACE_ANIMAL: return "animal";
    default: return "unknown";
    }
}

static RaceGroup raceGroupFor(Character* c)
{
    if (!readable(c, 8)) return RACE_UNKNOWN;
    if (c->isAnimal()) return RACE_ANIMAL;

    RaceData* race = c->getRace();
    if (!readable(race, 8)) return RACE_UNKNOWN;
    if (race->robot) return RACE_SKELETON;

    GameData* data = race->data;
    std::string key;
    if (readable(data, 8))
        key = toLower(data->name + " " + data->stringID);

    if (key.find("shek") != std::string::npos) return RACE_SHEK;
    if (key.find("hive") != std::string::npos || key.find("hiver") != std::string::npos) return RACE_HIVER;
    if (key.find("skeleton") != std::string::npos || key.find("robot") != std::string::npos) return RACE_SKELETON;
    if (key.find("greenlander") != std::string::npos ||
        key.find("scorchlander") != std::string::npos ||
        key.find("human") != std::string::npos)
        return RACE_HUMAN;

    return c->isHuman() ? RACE_HUMAN : RACE_UNKNOWN;
}

static std::string raceDataSummary(Character* c)
{
    if (!readable(c, 8)) return "race=null";

    RaceData* race = c->getRace();
    if (!readable(race, 8)) return "race=unreadable";

    GameData* data = race->data;
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "raceName=%s raceId=%s raceType=%d raceGroup=%p robot=%d",
        readable(data, 8) ? data->name.c_str() : "",
        readable(data, 8) ? data->stringID.c_str() : "",
        readable(data, 8) ? (int)data->type : -1,
        race->raceGroup,
        (int)race->robot);
    return std::string(buf);
}

static Character* currentTradePlayerCharacter()
{
    RootObject* player = gTrade.player;
    if (!readable(player, 8)) return NULL;
    if (!isCharacterType(player)) return NULL;
    return (Character*)player;
}

static RootObject* currentMoneyTradeShopkeeper()
{
    if (!InventoryGUI::isTradingForMoney_static()) return NULL;
    Character* npc = InventoryGUI::getNPCTrader();
    if (readable(npc, 8)) return (RootObject*)npc;
    return NULL;
}

static bool isShopKeeperObject(RootObject* obj)
{
    if (!readable(obj, 8)) return false;
    if (obj->getDataType() == SHOP_TRADER_CLASS) return true;

    RootObject* npc = (RootObject*)InventoryGUI::getNPCTrader();
    if (sameTradeObject(obj, npc)) return true;

    if (isCharacterType(obj))
        return isTraderPlatoon((Character*)obj);
    return false;
}

static bool isCurrentTradeShopkeeper(RootObject* obj)
{
    if (!isShopKeeperObject(obj)) return false;
    RootObject* shop = currentMoneyTradeShopkeeper();
    return sameTradeObject(obj, shop);
}

static bool isPlayerCharacterObject(RootObject* obj)
{
    if (!readable(obj, 8)) return false;
    if (!isCharacterType(obj)) return false;
    return ((Character*)obj)->isWithThePlayer();
}

static RootObject* itemInventoryOwner(Item* item)
{
    if (!readable(item, 8)) return NULL;
    const hand& h = item->getInventoryWeAreIn();
    RootObject* obj = h.getRootObject();
    if (readable(obj, 8)) return obj;

    Inventory* inv = item->getInventory();
    return inventoryOwner(inv);
}

static RootObject* itemProperOwner(Item* item)
{
    if (!readable(item, 8)) return NULL;
    const hand& h = item->getProperOwner();
    RootObject* obj = h.getRootObject();
    return readable(obj, 8) ? obj : NULL;
}

static bool itemBelongsToPlayerSide(Item* item, RootObject* inventorySide)
{
    if (isPlayerCharacterObject(inventorySide)) return true;
    return isPlayerCharacterObject(itemProperOwner(item));
}

static bool activeTradeHadPlayerItem(Item* item)
{
    if (!readable(item, 8)) return false;
    return gTrade.playerItems.find(item) != gTrade.playerItems.end();
}

static bool activeTradeHadShopItem(Item* item)
{
    if (!readable(item, 8)) return false;
    return gTrade.shopItems.find(item) != gTrade.shopItems.end();
}

// Some containers report their inventory owner as the item itself. That is
// normal for backpacks, but it hides the player as the source owner during a
// sale, so we treat self-owned backpack containers as player-side candidates
// only when they were not present in the shop snapshot at trade-open.
static bool itemOwnsItsInventory(Item* item, RootObject* sourceOwner)
{
    if (!readable(item, 8) || !readable(sourceOwner, 8)) return false;
    return ((RootObject*)item) == sourceOwner;
}

static bool itemLooksLikeBackpackContainer(Item* item)
{
    if (!readable(item, 8)) return false;
    if (item->itemFunction == ITEM_CONTAINER) return true;
    if (item->slotType == ATTACH_BACKPACK) return true;
    return classifyItem(item) == CAT_BACKPACK;
}

static int backpackContainerContentCount(Item* item)
{
    if (!itemLooksLikeBackpackContainer(item)) return 0;

    Inventory* inv = item->getInventory();
    if (!readable(inv, 8)) return 0;

    RootObject* owner = inventoryOwner(inv);
    if (!sameTradeObject(owner, (RootObject*)item)) return 0;

    return inv->getNumItems();
}

// Decide whether an inventory add/placement is the player selling to the active
// money-trade shop. This is intentionally stricter than "item is being added to
// an inventory": it must point at the current shop, come from the player side
// or the trade-open player snapshot, and then classify against that shop's
// vendor-list context.
static bool analysePlayerSaleToShop(Inventory* destInv, Item* item, int quantity,
                                    RootObject** destOwnerOut,
                                    RootObject** shopOut,
                                    RootObject** sourceOwnerOut,
                                    RootObject** properOwnerOut,
                                    Cat* catOut,
                                    Disp* dispOut,
                                    bool* destIsShopOut,
                                    bool* sourceIsPlayerOut,
                                    bool* ownerlessPlayerSaleOut)
{
    RootObject* destOwner = inventoryOwner(destInv);
    RootObject* shop = currentMoneyTradeShopkeeper();
    RootObject* sourceOwner = itemInventoryOwner(item);
    RootObject* properOwner = itemProperOwner(item);
    bool destIsShop = isCurrentTradeShopkeeper(destOwner);
    bool sourceOwnerless = !readable(sourceOwner, 8) && !readable(properOwner, 8);
    bool selfOwnedContainerSale = itemOwnsItsInventory(item, sourceOwner) &&
                                  itemLooksLikeBackpackContainer(item) &&
                                  destIsShop &&
                                  readable(gTrade.player, 8) &&
                                  sameTradeObject(shop, gTrade.shop) &&
                                  !activeTradeHadShopItem(item);
    bool ownerlessPlayerSale = cfg::treatOwnerlessTradeItemsAsPlayerSales &&
                               sourceOwnerless &&
                               destIsShop &&
                               readable(gTrade.player, 8) &&
                               sameTradeObject(shop, gTrade.shop) &&
                               !activeTradeHadShopItem(item);
    // The shop managing its own stock is not a player sale, and the clearest
    // tell is that the item already lives in the destination inventory. The
    // "arrange" button (ShopTraderInventorySection::autoArrange) re-places the
    // shop's sellable goods through the same inherited InventorySection::_addItem
    // we hook; those goods are already members of the shop's ShopTraderInventory,
    // whereas a genuine incoming sale item is not in the shop inventory yet.
    // Without this, arrange looks like an ownerless player sale and the shop's
    // own items get refused - which now destroys them (the block) instead of
    // duplicating them (the old gift). Never treat an item the destination
    // already holds as a player sale.

    // Of note, ownerless player sales happen when a player sells a container
    // like a backpack with items in it. In that sense the backpack owns the
    // items. So we must have some checks for ownerless items.
    bool itemAlreadyInDest = readable(destInv, 8) && readable(item, 8) && destInv->hasItem(item);
    bool sourceIsPlayer = !itemAlreadyInDest &&
                          (itemBelongsToPlayerSide(item, sourceOwner) ||
                           activeTradeHadPlayerItem(item) ||
                           selfOwnedContainerSale ||
                           ownerlessPlayerSale);

    if (destOwnerOut) *destOwnerOut = destOwner;
    if (shopOut) *shopOut = shop;
    if (sourceOwnerOut) *sourceOwnerOut = sourceOwner;
    if (properOwnerOut) *properOwnerOut = properOwner;
    if (destIsShopOut) *destIsShopOut = destIsShop;
    if (sourceIsPlayerOut) *sourceIsPlayerOut = sourceIsPlayer;
    if (ownerlessPlayerSaleOut) *ownerlessPlayerSaleOut = ownerlessPlayerSale;

    Cat cat = readable(item, 8) ? classifyItem(item) : CAT_OTHER;
    Disp disp = DISP_FULL;
    if (readable(shop, 8) && readable(item, 8))
    {
        ensureContext(shop, destInv);
        disp = dispositionFor(cat, itemStringID(item), NULL, item);
    }
    if (catOut) *catOut = cat;
    if (dispOut) *dispOut = disp;

    return quantity > 0 && destIsShop && sourceIsPlayer;
}

static void refusalFeedbackGuarded(Character* speaker, Item* item);

// ---------------------------------------------------------------------
//  Inventory::addItem refusal
//  This broad transfer path is gated to the active money trade shopkeeper
//  before classification/refusal details are logged.
// ---------------------------------------------------------------------
typedef bool (*InventoryAddItem_t)(Inventory*, Item*, int, bool, bool);
static InventoryAddItem_t InventoryAddItem_orig = NULL;
static bool gDbgAddItemFire = false;

static void logAddItemProbeImpl(const char* phase, Inventory* self, Item* itemToAdd,
                                int quantity, bool dropOnFail, bool destroyOnFail,
                                bool ret)
{
    if (!cfg::debug) return;
    if (!InventoryGUI::isTradingForMoney_static()) return;

    RootObject* destOwner = NULL;
    RootObject* shop = NULL;
    RootObject* sourceOwner = NULL;
    RootObject* properOwner = NULL;
    Cat cat = CAT_OTHER;
    Disp disp = DISP_FULL;
    bool destIsShop = false;
    bool sourceIsPlayer = false;
    bool ownerlessPlayerSale = false;
    bool playerSale = analysePlayerSaleToShop(self, itemToAdd, quantity,
        &destOwner, &shop, &sourceOwner, &properOwner, &cat, &disp,
        &destIsShop, &sourceIsPlayer, &ownerlessPlayerSale);

    if (!destIsShop && !isShopKeeperObject(destOwner)) return;

    static std::set<std::string> seen;
    if (seen.size() >= 240) return;
    char keyBuf[192];
    _snprintf_s(keyBuf, sizeof(keyBuf), _TRUNCATE, "%s|%p|%p|%p|%p|%d|%d|%d|%d",
        phase, self, itemToAdd, destOwner, sourceOwner, quantity,
        (int)playerSale, (int)ownerlessPlayerSale, (int)ret);
    std::string key(keyBuf);
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    std::string itemName = readable(itemToAdd, 8) ? safeName(itemToAdd) : std::string();
    std::string section = readable(itemToAdd, 8) ? itemToAdd->inventorySection : std::string();
    int itemQty = readable(itemToAdd, 8) ? itemToAdd->quantity : -1;
    int equipped = readable(itemToAdd, 8) ? (int)itemToAdd->isEquipped : 0;
    AttachSlot slot = readable(itemToAdd, 8) ? itemToAdd->slotType : ATTACH_NONE;

    char buf[896];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][add_probe] phase=%s money=1 dest=%p destOwner=%s shop=%s sourceOwner=%s properOwner=%s item=%s/%p section=%s qty=%d itemQty=%d cat=%s disp=%s equipped=%d slot=%s ret=%d drop=%d destroy=%d destIsShop=%d sourceIsPlayer=%d ownerlessPlayerSale=%d playerSale=%d",
        phase, self, objectSummary(destOwner).c_str(), objectSummary(shop).c_str(),
        objectSummary(sourceOwner).c_str(), objectSummary(properOwner).c_str(),
        itemName.c_str(), itemToAdd, section.c_str(), quantity, itemQty,
        catLabel(cat), dispLabel(disp), equipped, attachSlotLabel(slot),
        (int)ret, (int)dropOnFail,
        (int)destroyOnFail, (int)destIsShop, (int)sourceIsPlayer,
        (int)ownerlessPlayerSale, (int)playerSale);
    DebugLog(buf);
}

static void logAddItemProbeGuarded(const char* phase, Inventory* self, Item* itemToAdd,
                                   int quantity, bool dropOnFail, bool destroyOnFail,
                                   bool ret)
{
    __try
    {
        logAddItemProbeImpl(phase, self, itemToAdd, quantity, dropOnFail, destroyOnFail, ret);
    }
    __except (triiSehFilter("addItem.probe", GetExceptionInformation()))
    {
    }
}

static bool shouldRefuseAddItemImpl(Inventory* self, Item* itemToAdd, int quantity,
                                    Character** speaker)
{
    *speaker = NULL;
    if (!cfg::enabled || cfg::observeOnly) return false;

    RootObject* destOwner = NULL;
    RootObject* shop = NULL;
    RootObject* sourceOwner = NULL;
    RootObject* properOwner = NULL;
    Cat cat = CAT_OTHER;
    Disp disp = DISP_FULL;
    bool destIsShop = false;
    bool sourceIsPlayer = false;
    bool ownerlessPlayerSale = false;
    bool playerSale = analysePlayerSaleToShop(self, itemToAdd, quantity,
        &destOwner, &shop, &sourceOwner, &properOwner, &cat, &disp,
        &destIsShop, &sourceIsPlayer, &ownerlessPlayerSale);

    // Backpacks-with-contents are blocked so refused contents can't ride into a
    // shop inside an accepted bag - but an accept-all vendor takes the contents
    // too, so there's nothing to smuggle and the sale should go through.
    int protectedContainerItems = readable(itemToAdd, 8) ? backpackContainerContentCount(itemToAdd) : 0;
    bool protectContainerContents = playerSale && protectedContainerItems > 0 && !gCtx.isAcceptAllVendor;
    if (!playerSale || (disp != DISP_REFUSE && !protectContainerContents)) return false;
    Character* npc = InventoryGUI::getNPCTrader();
    if (readable(npc, 8)) *speaker = npc;

    if (cfg::debug)
    {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][add_refuse] item=%s/%p cat=%s equipped=%d slot=%s shop=%s source=%s proper=%s ownerlessPlayerSale=%d protectedContainerItems=%d",
            readable(itemToAdd, 8) ? safeName(itemToAdd).c_str() : "",
            itemToAdd, catLabel(cat),
            readable(itemToAdd, 8) ? (int)itemToAdd->isEquipped : 0,
            readable(itemToAdd, 8) ? attachSlotLabel(itemToAdd->slotType) : "unknown",
            objectSummary(shop).c_str(),
            objectSummary(sourceOwner).c_str(), objectSummary(properOwner).c_str(),
            (int)ownerlessPlayerSale, protectedContainerItems);
        DebugLog(buf);
    }
    return true;
}

static bool shouldRefuseAddItemGuarded(Inventory* self, Item* itemToAdd, int quantity,
                                       Character** speaker)
{
    __try
    {
        return shouldRefuseAddItemImpl(self, itemToAdd, quantity, speaker);
    }
    __except (triiSehFilter("addItem.refuse", GetExceptionInformation()))
    {
        *speaker = NULL;
        return false;
    }
}

static bool InventoryAddItem_hook(Inventory* self, Item* itemToAdd, int quantity,
                                  bool dropOnFail, bool destroyOnFail)
{
    onceLog(gDbgAddItemFire, "TradersRefuse: [bc] Inventory::addItem hook first fire");
    logAddItemProbeGuarded("pre", self, itemToAdd, quantity, dropOnFail, destroyOnFail, false);

    if (cfg::enableAddItemRefusal)
    {
        Character* speaker = NULL;
        if (shouldRefuseAddItemGuarded(self, itemToAdd, quantity, &speaker))
        {
            refusalFeedbackGuarded(speaker, itemToAdd);
            return false;
        }
    }

    bool ret = false;
    __try
    {
        ret = InventoryAddItem_orig(self, itemToAdd, quantity, dropOnFail, destroyOnFail);
    }
    __except (triiSehFilter("addItem.orig", GetExceptionInformation()))
    {
        return false;
    }

    logAddItemProbeGuarded("post", self, itemToAdd, quantity, dropOnFail, destroyOnFail, ret);
    return ret;
}

typedef bool (*InventoryTransferMouseItem_t)(Inventory*, Item*);
static InventoryTransferMouseItem_t InventoryTransferMouseItem_orig = NULL;
static bool gDbgTransferMouseItemFire = false;

static void logTransferMouseItemProbeImpl(const char* phase, Inventory* self, Item* item, bool ret)
{
    if (!cfg::debug) return;
    if (!InventoryGUI::isTradingForMoney_static()) return;

    RootObject* destOwner = NULL;
    RootObject* shop = NULL;
    RootObject* sourceOwner = NULL;
    RootObject* properOwner = NULL;
    Cat cat = CAT_OTHER;
    Disp disp = DISP_FULL;
    bool destIsShop = false;
    bool sourceIsPlayer = false;
    bool ownerlessPlayerSale = false;
    int quantity = readable(item, 8) && item->quantity > 0 ? item->quantity : 1;
    bool playerSale = analysePlayerSaleToShop(self, item, quantity,
        &destOwner, &shop, &sourceOwner, &properOwner, &cat, &disp,
        &destIsShop, &sourceIsPlayer, &ownerlessPlayerSale);

    if (!destIsShop && !isShopKeeperObject(destOwner)) return;

    static std::set<std::string> seen;
    if (seen.size() >= 160) return;
    char keyBuf[192];
    _snprintf_s(keyBuf, sizeof(keyBuf), _TRUNCATE, "%s|%p|%p|%p|%p|%d|%d|%d",
        phase, self, item, destOwner, sourceOwner, (int)playerSale, (int)disp, (int)ret);
    std::string key(keyBuf);
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    std::string itemName = readable(item, 8) ? safeName(item) : std::string();
    std::string section = readable(item, 8) ? item->inventorySection : std::string();
    int fn = readable(item, 8) ? (int)item->itemFunction : -1;

    char buf[896];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][transfer_mouse] phase=%s money=1 dest=%p destOwner=%s shop=%s sourceOwner=%s properOwner=%s item=%s/%p section=%s qty=%d fn=%d cat=%s disp=%s ret=%d destIsShop=%d sourceIsPlayer=%d ownerlessPlayerSale=%d playerSale=%d",
        phase, self, objectSummary(destOwner).c_str(), objectSummary(shop).c_str(),
        objectSummary(sourceOwner).c_str(), objectSummary(properOwner).c_str(),
        itemName.c_str(), item, section.c_str(), quantity, fn, catLabel(cat),
        dispLabel(disp), (int)ret, (int)destIsShop, (int)sourceIsPlayer,
        (int)ownerlessPlayerSale, (int)playerSale);
    DebugLog(buf);
}

static void logTransferMouseItemProbeGuarded(const char* phase, Inventory* self, Item* item, bool ret)
{
    __try
    {
        logTransferMouseItemProbeImpl(phase, self, item, ret);
    }
    __except (triiSehFilter("transferMouseItem.probe", GetExceptionInformation()))
    {
    }
}

static bool InventoryTransferMouseItem_hook(Inventory* self, Item* item)
{
    onceLog(gDbgTransferMouseItemFire, "TradersRefuse: [bc] Inventory::transferMouseItem hook first fire");
    logTransferMouseItemProbeGuarded("pre", self, item, false);

    if (cfg::enableTransferMouseItemRefusal)
    {
        Character* speaker = NULL;
        int quantity = readable(item, 8) && item->quantity > 0 ? item->quantity : 1;
        if (shouldRefuseAddItemGuarded(self, item, quantity, &speaker))
        {
            if (cfg::debug)
                DebugLog("[TRII][transfer_refuse] blocked refused drag/drop sale");
            refusalFeedbackGuarded(speaker, item);
            return false;
        }
    }

    bool ret = false;
    __try
    {
        ret = InventoryTransferMouseItem_orig(self, item);
    }
    __except (triiSehFilter("transferMouseItem.orig", GetExceptionInformation()))
    {
        return false;
    }

    logTransferMouseItemProbeGuarded("post", self, item, ret);
    return ret;
}

// Section placement fires after the mouse picked the item up, so a refused sale
// must be handed back or it vanishes from the player inventory until the engine
// catches up.
static bool restoreRefusedItemToPlayerImpl(Item* item, int quantity)
{
    if (!cfg::restoreRefusedDragDropItems) return false;
    if (!readable(item, 8)) return false;
    if (!readable(gTrade.player, 8)) return false;

    // Only ever hand an item back to the player if it genuinely originated on
    // the player's side of this trade. The shop re-sorting its own sellable
    // stock (the "arrange" button -> ShopTraderInventorySection::autoArrange,
    // which re-places through the inherited InventorySection::_addItem we hook)
    // looks like an ownerless drag/drop at the moment of placement. Without
    // this gate the shop's own goods (blueprints, notices, ...) get "restored"
    // into the player's inventory for free - and, since shop stock regenerates
    // on the next visit, farmed indefinitely. Never restore shop-owned items.
    RootObject* properOwner = itemProperOwner(item);
    bool playerOriginated = activeTradeHadPlayerItem(item) || isPlayerCharacterObject(properOwner);
    bool shopOriginated = activeTradeHadShopItem(item) || isCurrentTradeShopkeeper(properOwner);
    if (!playerOriginated || shopOriginated)
    {
        if (cfg::debug)
        {
            char skipBuf[512];
            _snprintf_s(skipBuf, sizeof(skipBuf), _TRUNCATE,
                "[TRII][restore_skip] not a player-originated sale; item=%s/%p playerOriginated=%d shopOriginated=%d properOwner=%s",
                safeName(item).c_str(), item, (int)playerOriginated, (int)shopOriginated,
                objectSummary(properOwner).c_str());
            DebugLog(skipBuf);
        }
        return false;
    }

    Inventory* playerInv = gTrade.player->getInventory();
    if (!readable(playerInv, 8)) return false;

    RootObject* currentOwner = itemInventoryOwner(item);
    if (sameTradeObject(currentOwner, gTrade.player))
        return true;

    // Route through the captured original, never the public addItem - the latter
    // re-enters our Inventory::_addItem hook. A null original means the hook never
    // installed, so there is nothing safe to hand the item back through.
    if (!InventoryAddItem_orig) return false;
    bool ret = InventoryAddItem_orig(playerInv, item, quantity > 0 ? quantity : 1, false, false);

    if (ret)
        gTrade.playerItems.insert(item);

    if (cfg::debug)
    {
        char buf[768];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][restore_refused] item=%s/%p qty=%d ret=%d player=%s playerInv=%p previousOwner=%s newOwner=%s",
            safeName(item).c_str(), item, quantity, (int)ret,
            objectSummary(gTrade.player).c_str(), playerInv,
            objectSummary(currentOwner).c_str(),
            objectSummary(itemInventoryOwner(item)).c_str());
        DebugLog(buf);
    }

    return ret;
}

static bool restoreRefusedItemToPlayerGuarded(Item* item, int quantity)
{
    __try
    {
        return restoreRefusedItemToPlayerImpl(item, quantity);
    }
    __except (triiSehFilter("restoreRefusedItem", GetExceptionInformation()))
    {
        return false;
    }
}

typedef void (*InventorySectionPlaceItem_t)(InventorySection*, Item*, int, int);
static InventorySectionPlaceItem_t InventorySectionPlaceItem_orig = NULL;
static bool gDbgSectionPlaceItemFire = false;

static void logSectionPlaceProbeImpl(const char* phase, InventorySection* self,
                                     Item* item, int x, int y, bool blocked)
{
    if (!cfg::debug) return;
    if (!InventoryGUI::isTradingForMoney_static()) return;

    Inventory* inv = readable(self, 8) ? self->getInventory() : NULL;
    RootObject* destOwner = NULL;
    RootObject* shop = NULL;
    RootObject* sourceOwner = NULL;
    RootObject* properOwner = NULL;
    Cat cat = CAT_OTHER;
    Disp disp = DISP_FULL;
    bool destIsShop = false;
    bool sourceIsPlayer = false;
    bool ownerlessPlayerSale = false;
    int quantity = readable(item, 8) && item->quantity > 0 ? item->quantity : 1;
    int protectedContainerItems = readable(item, 8) ? backpackContainerContentCount(item) : 0;
    bool playerSale = analysePlayerSaleToShop(inv, item, quantity,
        &destOwner, &shop, &sourceOwner, &properOwner, &cat, &disp,
        &destIsShop, &sourceIsPlayer, &ownerlessPlayerSale);

    if (!destIsShop && !isShopKeeperObject(destOwner)) return;

    static std::set<std::string> seen;
    if (seen.size() >= 180) return;
    char keyBuf[192];
    _snprintf_s(keyBuf, sizeof(keyBuf), _TRUNCATE, "%s|%p|%p|%p|%d|%d|%d|%d",
        phase, self, item, destOwner, x, y, (int)disp, (int)blocked);
    std::string key(keyBuf);
    if (seen.find(key) != seen.end()) return;
    seen.insert(key);

    std::string itemName = readable(item, 8) ? safeName(item) : std::string();
    std::string itemSection = readable(item, 8) ? item->inventorySection : std::string();
    std::string destSection = readable(self, 8) ? self->name : std::string();
    int fn = readable(item, 8) ? (int)item->itemFunction : -1;

    char buf[1024];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][section_place] phase=%s blocked=%d section=%s pos=%d,%d inv=%p destOwner=%s shop=%s sourceOwner=%s properOwner=%s item=%s/%p itemSection=%s qty=%d fn=%d cat=%s disp=%s protectedContainerItems=%d destIsShop=%d sourceIsPlayer=%d ownerlessPlayerSale=%d playerSale=%d",
        phase, (int)blocked, destSection.c_str(), x, y, inv,
        objectSummary(destOwner).c_str(), objectSummary(shop).c_str(),
        objectSummary(sourceOwner).c_str(), objectSummary(properOwner).c_str(),
        itemName.c_str(), item, itemSection.c_str(), quantity, fn, catLabel(cat),
        dispLabel(disp), protectedContainerItems, (int)destIsShop, (int)sourceIsPlayer,
        (int)ownerlessPlayerSale, (int)playerSale);
    DebugLog(buf);
}

static void logSectionPlaceProbeGuarded(const char* phase, InventorySection* self,
                                        Item* item, int x, int y, bool blocked)
{
    __try
    {
        logSectionPlaceProbeImpl(phase, self, item, x, y, blocked);
    }
    __except (triiSehFilter("sectionPlace.probe", GetExceptionInformation()))
    {
    }
}

// Direct grid placement is the reliable drag/drop enforcement point. The
// higher-level mouse hooks can observe or catch earlier paths, but this hook is
// the last safe place before a refused item lands in the shop inventory.
static void InventorySectionPlaceItem_hook(InventorySection* self, Item* item, int x, int y)
{
    onceLog(gDbgSectionPlaceItemFire, "TradersRefuse: [bc] InventorySection::_addItem hook first fire");
    logSectionPlaceProbeGuarded("pre", self, item, x, y, false);

    if (cfg::enableSectionPlaceRefusal && readable(self, 8))
    {
        Inventory* inv = self->getInventory();
        Character* speaker = NULL;
        int quantity = readable(item, 8) && item->quantity > 0 ? item->quantity : 1;
        if (shouldRefuseAddItemGuarded(inv, item, quantity, &speaker))
        {
            logSectionPlaceProbeGuarded("refuse", self, item, x, y, true);
            restoreRefusedItemToPlayerGuarded(item, quantity);
            refusalFeedbackGuarded(speaker, item);
            return;
        }
    }

    __try
    {
        InventorySectionPlaceItem_orig(self, item, x, y);
    }
    __except (triiSehFilter("sectionPlace.orig", GetExceptionInformation()))
    {
        return;
    }

    logSectionPlaceProbeGuarded("post", self, item, x, y, false);
}

typedef bool (*ShopTraderInventoryAddItemInternal_t)(ShopTraderInventory*, Item*, int);
static ShopTraderInventoryAddItemInternal_t ShopTraderInventoryAddItemInternal_orig = NULL;
static bool gDbgShopAddItemFire = false;

static bool ShopTraderInventoryAddItemInternal_hook(ShopTraderInventory* self, Item* itemToAdd, int quantity)
{
    onceLog(gDbgShopAddItemFire, "TradersRefuse: [bc] ShopTraderInventory::_addItem hook first fire");
    logAddItemProbeGuarded("shop_pre", self, itemToAdd, quantity, false, false, false);

    if (cfg::enableAddItemRefusal)
    {
        Character* speaker = NULL;
        if (shouldRefuseAddItemGuarded(self, itemToAdd, quantity, &speaker))
        {
            refusalFeedbackGuarded(speaker, itemToAdd);
            return false;
        }
    }

    bool ret = false;
    __try
    {
        ret = ShopTraderInventoryAddItemInternal_orig(self, itemToAdd, quantity);
    }
    __except (triiSehFilter("shopAddItem.orig", GetExceptionInformation()))
    {
        return false;
    }

    logAddItemProbeGuarded("shop_post", self, itemToAdd, quantity, false, false, ret);
    return ret;
}

typedef bool (*ShopTraderInventorySectionAddItem_t)(ShopTraderInventorySection*, Item*, int);
static ShopTraderInventorySectionAddItem_t ShopTraderInventorySectionAddItem_orig = NULL;
static bool gDbgShopSectionAddItemFire = false;

static bool ShopTraderInventorySectionAddItem_hook(ShopTraderInventorySection* self, Item* itemToAdd, int quantity)
{
    onceLog(gDbgShopSectionAddItemFire, "TradersRefuse: [bc] ShopTraderInventorySection::addItem hook first fire");

    Inventory* inv = NULL;
    if (readable(self, 8))
        inv = self->getInventory();

    logAddItemProbeGuarded("shop_section_pre", inv, itemToAdd, quantity, false, false, false);

    if (cfg::enableAddItemRefusal && readable(inv, 8))
    {
        Character* speaker = NULL;
        if (shouldRefuseAddItemGuarded(inv, itemToAdd, quantity, &speaker))
        {
            refusalFeedbackGuarded(speaker, itemToAdd);
            return false;
        }
    }

    bool ret = false;
    __try
    {
        ret = ShopTraderInventorySectionAddItem_orig(self, itemToAdd, quantity);
    }
    __except (triiSehFilter("shopSectionAddItem.orig", GetExceptionInformation()))
    {
        return false;
    }

    logAddItemProbeGuarded("shop_section_post", inv, itemToAdd, quantity, false, false, ret);
    return ret;
}

// autoArrange re-places the shop's own stock through the hooked _addItem while the
// item is transiently ownerless. Snapshotting self->getInventory() here (before it
// mutates) lets activeTradeHadShopItem() recognise those items so they aren't
// misread as a player sale; it also catches goods sold earlier in this trade.
typedef void (*ShopTraderSectionAutoArrange_t)(ShopTraderInventorySection*);
static ShopTraderSectionAutoArrange_t ShopTraderSectionAutoArrange_orig = NULL;
static bool gDbgShopAutoArrangeFire = false;

// Defined later with the trade-context helpers; forward-declared for the hook.
static void snapshotTradeItems(Inventory* inventory, std::set<Item*>& out);

static void snapshotArrangeStockImpl(ShopTraderInventorySection* self)
{
    if (!readable(self, 8)) return;
    Inventory* inv = self->getInventory();
    if (!readable(inv, 8)) return;
    size_t before = gTrade.shopItems.size();
    snapshotTradeItems(inv, gTrade.shopItems);
    if (cfg::debug)
    {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][arrange_snap] section=%p inv=%p added=%d shopItemsTotal=%d",
            self, inv, (int)(gTrade.shopItems.size() - before), (int)gTrade.shopItems.size());
        DebugLog(buf);
    }
}

static void snapshotArrangeStockGuarded(ShopTraderInventorySection* self)
{
    __try
    {
        snapshotArrangeStockImpl(self);
    }
    __except (triiSehFilter("shopAutoArrange.snapshot", GetExceptionInformation()))
    {
    }
}

static void ShopTraderSectionAutoArrange_hook(ShopTraderInventorySection* self)
{
    onceLog(gDbgShopAutoArrangeFire, "TradersRefuse: [bc] ShopTraderInventorySection::autoArrange hook first fire");
    snapshotArrangeStockGuarded(self);

    __try
    {
        ShopTraderSectionAutoArrange_orig(self);
    }
    __except (triiSehFilter("shopAutoArrange.orig", GetExceptionInformation()))
    {
    }
}

static bool refusalModeIs(const char* mode)
{
    return std::string(cfg::refusalMode) == mode;
}

static const char* kSuicideNoteItemId = "49494-dialogue.mod";

static bool itemIsSuicideNote(Item* item)
{
    return itemStringID(item) == kSuicideNoteItemId;
}

static std::map<Character*, int> gRefusalSpeechCounts;
static std::map<Character*, DWORD> gRefusalSpeechLastMs;

static void resetRefusalSpeechState()
{
    gRefusalSpeechCounts.clear();
    gRefusalSpeechLastMs.clear();
    if (cfg::debug)
        DebugLog("[TRII][refusal_speech] reset state for new trade");
}

static const char* genericRefusalLineForCount(int count)
{
    switch (count)
    {
    case 0: return "I don't want that junk.";
    case 1: return "I said I don't buy that.";
    case 2: return "Try selling it somewhere else.";
    default: return "Still no.";
    }
}

static const char* raceRefusalLineForCount(RaceGroup speakerRace, RaceGroup targetRace, int count)
{
    if (speakerRace == RACE_SHEK && targetRace == RACE_HUMAN)
    {
        switch (count)
        {
        case 0: return "Getting tired of you, flatskin.";
        case 1: return "Your junk is still junk.";
        case 2: return "Bring something worth a Shek's time.";
        default: return "Still no, flatskin.";
        }
    }
    if (speakerRace == RACE_SHEK && targetRace == RACE_HIVER)
    {
        switch (count)
        {
        case 0: return "Bring me something better, insect-man.";
        case 1: return "Your items are worthless, bug.";
        case 2: return "Try a scrapyard, bug.";
        default: return "I have no interest in your toys, insect-man.";
        }
    }
    if (speakerRace == RACE_HUMAN && targetRace == RACE_SHEK)
    {
        switch (count)
        {
        case 0: return "Not buying that, warrior.";
        case 1: return "You heard me the first time.";
        case 2: return "Take it to someone who needs scrap.";
        default: return "Still not buying it.";
        }
    }
    if (speakerRace == RACE_HIVER)
    {
        switch (count)
        {
        case 0: return "No deal, no deal.";
        case 1: return "Bad trade for shop.";
        case 2: return "Take wrong goods away.";
        default: return "Still no deal.";
        }
    }
    if (speakerRace == RACE_SKELETON)
    {
        switch (count)
        {
        case 0: return "Not interested.";
        case 1: return "This item is not valuable to me.";
        case 2: return "I do not want this.";
        default: return "No.";
        }
    }
    return genericRefusalLineForCount(count);
}

static const char* suicideNoteRefusalLineForRace(RaceGroup speakerRace, int count)
{
    if (speakerRace == RACE_HUMAN)
    {
        switch (count)
        {
        case 0: return "I'm not buying someone's last words.";
        case 1: return "That belongs with the dead.";
        default: return "Put it away.";
        }
    }
    if (speakerRace == RACE_SHEK)
    {
        switch (count)
        {
        case 0: return "A coward's last writings does not interest me.";
        case 1: return "This paper is worthless to me.";
        default: return "...";
        }
    }
    if (speakerRace == RACE_HIVER)
    {
        switch (count)
        {
        case 0: return "No trade for death-note, no no.";
        case 1: return "Shop cannot buy this cursed thing.";
        default: return "Still no trade.";
        }
    }
    if (speakerRace == RACE_SKELETON)
    {
        switch (count)
        {
        case 0: return "Please put that away.";
        case 1: return "I will not buy this.";
        default: return "Absolutely not.";
        }
    }
    return "No. I can't accept this...";
}

static void speakRefusal(Character* speaker, Item* item)
{
    if (!speaker) return;
    if (refusalModeIs("silent")) return;
    if (!speaker->canSpeakNormally()) return;

    Character* target = currentTradePlayerCharacter();
    RaceGroup speakerRace = raceGroupFor(speaker);
    RaceGroup targetRace = raceGroupFor(target);

    DWORD now = GetTickCount();
    DWORD last = gRefusalSpeechLastMs[speaker];
    if (last != 0 && (DWORD)(now - last) < (DWORD)cfg::refusalSpeechCooldownMs)
    {
        if (cfg::debug)
        {
            char skipBuf[640];
            _snprintf_s(skipBuf, sizeof(skipBuf), _TRUNCATE,
                "[TRII][refusal_speech_skip] speaker=%s target=%s speakerRace=%s targetRace=%s ageMs=%lu cooldownMs=%d",
                objectSummary((RootObject*)speaker).c_str(), objectSummary((RootObject*)target).c_str(),
                raceGroupLabel(speakerRace), raceGroupLabel(targetRace),
                (unsigned long)(DWORD)(now - last), cfg::refusalSpeechCooldownMs);
            DebugLog(skipBuf);
        }
        return;
    }
    gRefusalSpeechLastMs[speaker] = now;

    int count = gRefusalSpeechCounts[speaker]++;
    bool suicideNote = readable(item, 8) && itemIsSuicideNote(item);
    const char* line = suicideNote ?
        suicideNoteRefusalLineForRace(speakerRace, count) :
        raceRefusalLineForCount(speakerRace, targetRace, count);
    speaker->sayALine(std::string(line), true);
    if (cfg::debug)
    {
        std::string speakerRaceData = raceDataSummary(speaker);
        std::string targetRaceData = raceDataSummary(target);
        char buf[960];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][refusal_speech] speaker=%s target=%s speakerRace=%s targetRace=%s count=%d suicideNote=%d line=%s speakerData={%s} targetData={%s}",
            objectSummary((RootObject*)speaker).c_str(), objectSummary((RootObject*)target).c_str(),
            raceGroupLabel(speakerRace), raceGroupLabel(targetRace), count, (int)suicideNote, line,
            speakerRaceData.c_str(), targetRaceData.c_str());
        DebugLog(buf);
    }
}

static void closeTradeWindowForRefusal()
{
    if (!refusalModeIs("eject")) return;
    if (!readable(gui, 8))
    {
        triiNote("refusal.close: global gui not readable");
        return;
    }

    if (cfg::debug)
        DebugLog("[TRII][refusal] closing trade window via ForgottenGUI::closeTradeWindow");
    gui->closeTradeWindow();
}

static const char* tradeWindowTypeLabel(TradeWindowType tradeType)
{
    switch (tradeType)
    {
    case TW_OFF: return "off";
    case TW_MONEY_TRADING: return "money";
    case TW_LOOTING: return "looting";
    case TW_AUTO: return "auto";
    default: return "unknown";
    }
}

static void logTradeWindowItems(const char* side, Inventory* inventory)
{
    if (!cfg::debug) return;
    if (!readable(inventory, 8)) return;

    const lektor<Item*>& allItems = inventory->getAllItems();
    uint32_t count = allItems.size();
    uint32_t limit = count < 80 ? count : 80;

    for (uint32_t i = 0; i < limit; ++i)
    {
        Item* item = allItems[i];
        if (!readable(item, 8)) continue;

        Cat cat = classifyItem(item);
        char buf[640];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][trade_window_item] side=%s index=%u item=%s/%p section=%s fn=%d cat=%s equipped=%d slot=%s",
            side, i, safeName(item).c_str(), item, item->inventorySection.c_str(),
            (int)item->itemFunction, catLabel(cat), (int)item->isEquipped,
            attachSlotLabel(item->slotType));
        DebugLog(buf);
    }

    if (count > limit)
    {
        char buf[160];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][trade_window_item] side=%s truncated=%u total=%u",
            side, count - limit, count);
        DebugLog(buf);
    }
}

static void snapshotTradeItems(Inventory* inventory, std::set<Item*>& out)
{
    if (!readable(inventory, 8)) return;
    const lektor<Item*>& allItems = inventory->getAllItems();
    uint32_t count = allItems.size();
    uint32_t limit = count < 240 ? count : 240;
    for (uint32_t i = 0; i < limit; ++i)
    {
        Item* item = allItems[i];
        if (readable(item, 8)) out.insert(item);
    }
}

// Capture the player/shop item pointers at trade-open. The snapshot lets us
// classify equipped, ownerless, and self-owned container transfers without
// trusting the transient owner pointers seen during drag/drop.
static void updateActiveTradeContext(ForgottenGUI* self, RootObject* a, RootObject* b,
                                     TradeWindowType tradeType)
{
    gTrade.clear();
    // Invalidate the pointer-keyed shop cache too: a freed shopkeeper whose
    // address is later reused would otherwise hit gCtx and apply stale rules.
    gCtx.clear();
    if (tradeType != TW_MONEY_TRADING) return;
    if (!InventoryGUI::isTradingForMoney_static()) return;

    RootObject* npc = (RootObject*)InventoryGUI::getNPCTrader();
    if (!readable(npc, 8)) return;

    RootObject* player = NULL;
    RootObject* shop = NULL;
    if (sameTradeObject(a, npc)) { shop = a; player = b; }
    else if (sameTradeObject(b, npc)) { shop = b; player = a; }
    else { shop = npc; player = isPlayerCharacterObject(a) ? a : b; }

    Inventory* playerInv = readable(player, 8) ? player->getInventory() : NULL;
    Inventory* shopInv = readable(shop, 8) ? shop->getInventory() : NULL;
    gTrade.player = player;
    gTrade.shop = shop;
    resetRefusalSpeechState();
    snapshotTradeItems(playerInv, gTrade.playerItems);
    snapshotTradeItems(shopInv, gTrade.shopItems);
    // Shop *sellable* stock (in a separate ShopTraderInventory) is captured at
    // arrange time by the ShopTraderInventorySection::autoArrange hook instead -
    // that reads the stock inventory directly and also catches goods sold earlier
    // this trade, which a trade-open snapshot would miss.

    if (cfg::debug)
    {
        char buf[640];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][trade_ctx] gui=%p player=%s shop=%s playerInv=%p playerItems=%u shopInv=%p shopItems=%u",
            self, objectSummary(player).c_str(), objectSummary(shop).c_str(),
            playerInv, (unsigned int)gTrade.playerItems.size(),
            shopInv, (unsigned int)gTrade.shopItems.size());
        DebugLog(buf);
    }
}

static void logTradeWindowOpen(const char* phase, ForgottenGUI* self, RootObject* a, RootObject* b,
                               TradeWindowType tradeType)
{
    if (!cfg::debug) return;

    Inventory* aInv = readable(a, 8) ? a->getInventory() : NULL;
    Inventory* bInv = readable(b, 8) ? b->getInventory() : NULL;
    int aItems = readable(aInv, 8) ? (int)aInv->getAllItems().size() : -1;
    int bItems = readable(bInv, 8) ? (int)bInv->getAllItems().size() : -1;

    char buf[768];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][trade_window] phase=%s gui=%p type=%s a=%s b=%s aInv=%p aItems=%d bInv=%p bItems=%d npc=%s money=%d",
        phase, self, tradeWindowTypeLabel(tradeType), objectSummary(a).c_str(), objectSummary(b).c_str(),
        aInv, aItems, bInv, bItems, objectSummary((RootObject*)InventoryGUI::getNPCTrader()).c_str(),
        (int)(InventoryGUI::isTradingForMoney_static() != NULL));
    DebugLog(buf);

    if (tradeType == TW_MONEY_TRADING)
    {
        logTradeWindowItems("a", aInv);
        logTradeWindowItems("b", bInv);
    }
}

// Trade-window open is our stable reset point for per-trade state. Guarded like
// every other hook: a fault in engine setup or our context capture fails open.
void (*_showTradeWindow_orig)(ForgottenGUI* thisptr, RootObject* a, RootObject* b, TradeWindowType tradeType);
void _showTradeWindow_hook(ForgottenGUI* thisptr, RootObject* a, RootObject* b, TradeWindowType tradeType)
{
    __try
    {
        logTradeWindowOpen("pre", thisptr, a, b, tradeType);
    }
    __except (triiSehFilter("showTradeWindow.logPre", GetExceptionInformation())) {}

    __try
    {
        _showTradeWindow_orig(thisptr, a, b, tradeType);
    }
    __except (triiSehFilter("showTradeWindow.orig", GetExceptionInformation()))
    {
        return;
    }

    __try
    {
        updateActiveTradeContext(thisptr, a, b, tradeType);
        logTradeWindowOpen("post", thisptr, a, b, tradeType);
    }
    __except (triiSehFilter("showTradeWindow.ctx", GetExceptionInformation())) {}
}

static void refusalFeedbackGuarded(Character* speaker, Item* item)
{
    __try
    {
        speakRefusal(speaker, item);
        closeTradeWindowForRefusal();
    }
    __except (triiSehFilter("block.feedback", GetExceptionInformation()))
    {
        /* best-effort */
    }
}

static bool shouldRefuseMousePlaceImpl(InventoryGUI* self, const std::string& sectionName,
                                       const MyGUI::types::TPoint<int>& mousePos,
                                       Character** speaker)
{
    *speaker = NULL;
    if (!cfg::enabled || cfg::observeOnly) return false;
    if (!readable(self, 8)) return false;
    if (cfg::requireMoneyTrade && !InventoryGUI::isTradingForMoney_static()) return false;

    Inventory* destInv = self->getInventory();
    Item* item = IGUIHookAccess::mouseItemFor(self);
    if (!readable(destInv, 8) || !readable(item, 8)) return false;

    RootObject* destOwner = NULL;
    RootObject* shop = NULL;
    RootObject* sourceOwner = NULL;
    RootObject* properOwner = NULL;
    Cat cat = CAT_OTHER;
    Disp disp = DISP_FULL;
    bool destIsShop = false;
    bool sourceIsPlayer = false;
    bool ownerlessPlayerSale = false;
    int quantity = item->quantity > 0 ? item->quantity : 1;
    int protectedContainerItems = backpackContainerContentCount(item);
    bool playerSale = analysePlayerSaleToShop(destInv, item, quantity,
        &destOwner, &shop, &sourceOwner, &properOwner, &cat, &disp,
        &destIsShop, &sourceIsPlayer, &ownerlessPlayerSale);

    if (cfg::debug && (destIsShop || isShopKeeperObject(destOwner)))
    {
        char buf[960];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][mouse_place] section=%s pos=%d,%d dest=%p destOwner=%s shop=%s sourceOwner=%s properOwner=%s item=%s/%p itemSection=%s qty=%d cat=%s disp=%s protectedContainerItems=%d destIsShop=%d sourceIsPlayer=%d ownerlessPlayerSale=%d playerSale=%d",
            sectionName.c_str(), mousePos.left, mousePos.top, destInv,
            objectSummary(destOwner).c_str(), objectSummary(shop).c_str(),
            objectSummary(sourceOwner).c_str(), objectSummary(properOwner).c_str(),
            safeName(item).c_str(), item, item->inventorySection.c_str(), quantity,
            catLabel(cat), dispLabel(disp), protectedContainerItems, (int)destIsShop, (int)sourceIsPlayer,
            (int)ownerlessPlayerSale, (int)playerSale);
        DebugLog(buf);
    }

    // See shouldRefuseAddItemImpl: accept-all vendors take bag contents too, so
    // the container-contents block doesn't apply to them.
    bool protectContainerContents = protectedContainerItems > 0 && !gCtx.isAcceptAllVendor;
    if (!playerSale || (disp != DISP_REFUSE && !protectContainerContents)) return false;

    Character* npc = InventoryGUI::getNPCTrader();
    if (readable(npc, 8)) *speaker = npc;
    return true;
}

static bool shouldRefuseMousePlaceGuarded(InventoryGUI* self, const std::string& sectionName,
                                          const MyGUI::types::TPoint<int>& mousePos,
                                          Character** speaker)
{
    __try
    {
        return shouldRefuseMousePlaceImpl(self, sectionName, mousePos, speaker);
    }
    __except (triiSehFilter("mousePlace.refuse", GetExceptionInformation()))
    {
        *speaker = NULL;
        return false;
    }
}

static bool gDbgPlaceItemFromMouseFire = false;

static bool PlaceItemFromMouse_hook(InventoryGUI* self, const std::string& sectionName,
                                    const MyGUI::types::TPoint<int>& mousePos)
{
    onceLog(gDbgPlaceItemFromMouseFire, "TradersRefuse: [bc] InventoryGUI::placeItemFromMouse hook first fire");
    if (cfg::enablePlaceItemFromMouseRefusal)
    {
        Character* speaker = NULL;
        if (shouldRefuseMousePlaceGuarded(self, sectionName, mousePos, &speaker))
        {
            if (cfg::debug)
                DebugLog("[TRII][mouse_place_refuse] blocked refused drag/drop sale");
            refusalFeedbackGuarded(speaker, readable(self, 8) ? IGUIHookAccess::mouseItemFor(self) : NULL);
            return false;
        }
    }

    bool ret = false;
    __try
    {
        ret = PlaceItemFromMouse_orig(self, sectionName, mousePos);
    }
    __except (triiSehFilter("placeItemFromMouse.orig", GetExceptionInformation()))
    {
        return false;
    }
    return ret;
}

// ---------------------------------------------------------------------
//  ENTRY POINT
// ---------------------------------------------------------------------
static void logAddr(const char* what, intptr_t addr, bool ok)
{
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "TradersRefuse: hook %s addr=%p install=%s",
        what, (void*)addr, ok ? "OK" : "FAILED");
    DebugLog(buf);
}

__declspec(dllexport) void startPlugin()
{
    buildRules();

    if (cfg::enableValueHook)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&InventoryItemBase::_NV_getValueSingle);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, GetValueSingle_hook, &GetValueSingle_orig));
        logAddr("getValueSingle", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook getValueSingle");

        intptr_t allAddr = KenshiLib::GetRealAddress(&InventoryItemBase::_NV_getValueAll);
        bool allOk = (KenshiLib::SUCCESS == KenshiLib::AddHook(allAddr, GetValueAll_hook, &GetValueAll_orig));
        logAddr("getValueAll", allAddr, allOk);
        if (!allOk) ErrorLog("TradersRefuse: could not hook getValueAll");
    }

    if (cfg::enablePlaceItemFromMouseHook)
    {
        intptr_t addr = IGUIHookAccess::placeItemFromMouseAddr();
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, PlaceItemFromMouse_hook, &PlaceItemFromMouse_orig));
        logAddr("InventoryGUI::placeItemFromMouse", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook InventoryGUI::placeItemFromMouse");
    }

    if (cfg::enableAddItemHooks)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&Inventory::_NV_addItem);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, InventoryAddItem_hook, &InventoryAddItem_orig));
        logAddr("Inventory::addItem", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook Inventory::addItem");
    }
    if (cfg::enableTransferMouseItemHook)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&Inventory::transferMouseItem);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, InventoryTransferMouseItem_hook, &InventoryTransferMouseItem_orig));
        logAddr("Inventory::transferMouseItem", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook Inventory::transferMouseItem");
    }
    if (cfg::enableSectionPlaceHook)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&InventorySection::_NV__addItem);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, InventorySectionPlaceItem_hook, &InventorySectionPlaceItem_orig));
        logAddr("InventorySection::_addItem", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook InventorySection::_addItem");
    }
    if (cfg::enableAddItemHooks)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&ShopTraderInventory::_NV__addItem);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, ShopTraderInventoryAddItemInternal_hook, &ShopTraderInventoryAddItemInternal_orig));
        logAddr("ShopTraderInventory::_addItem", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook ShopTraderInventory::_addItem");
    }
    if (cfg::enableAddItemHooks)
    {
        intptr_t addr = KenshiLib::GetRealAddress(&ShopTraderInventorySection::_NV_addItem);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, ShopTraderInventorySectionAddItem_hook, &ShopTraderInventorySectionAddItem_orig));
        logAddr("ShopTraderInventorySection::addItem", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook ShopTraderInventorySection::addItem");
    }
    {
        intptr_t addr = KenshiLib::GetRealAddress(&ShopTraderInventorySection::_NV_autoArrange);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, ShopTraderSectionAutoArrange_hook, &ShopTraderSectionAutoArrange_orig));
        logAddr("ShopTraderInventorySection::autoArrange", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook ShopTraderInventorySection::autoArrange");
    }

    {
        intptr_t addr = KenshiLib::GetRealAddress(&ForgottenGUI::_showTradeWindow);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, &_showTradeWindow_hook, &_showTradeWindow_orig));
        logAddr("ForgottenGUI::_showTradeWindow", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook ForgottenGUI::_showTradeWindow");
    }

    DebugLog("TradersRefuse: native plugin loaded");
}
