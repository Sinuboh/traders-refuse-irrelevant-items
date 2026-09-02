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
//    - exact stock name/category matches -> full acceptance
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
    static const int    refusedPreviewValue = 1;      // zero-value refused sales looked crash-prone in the UI
    static const bool   refuseUnknown       = false;  // false => allow unclassified (OTHER) items
    static const bool   useStockAcceptance  = true;   // the shop's real stock expands acceptance
    static const bool   scanAllVendorRefs   = true;   // inspect FCS VENDOR_LIST refs on squad/shop/building data
    static const bool   acceptAllWhenNoVendorList = true; // missing vendor data means leave vanilla trading alone
    static const double offListFoodMult     = 0.6;    // most shops will still buy food, just at a worse price
    static const bool   logVendorListItems  = true;   // dump exact FCS vendor-list stock refs once per run
    static const int    maxVendorItemLogs   = 240;    // enough to learn shops, bounded to avoid log floods
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

static bool looksLikeMapName(const std::string& name)
{
    std::string lower = toLower(name);
    return lower.find("map") != std::string::npos;
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
// Base valuation path only sees InventoryItemBase; use itemFunction first,
// then the inventorySection substring, then OTHER (== classifyValueItem()).
static Cat classifyByFunctionAndSection(InventoryItemBase* item)
{
    if (!item) return CAT_OTHER;
    bool mapped = false;
    Cat c = funcToCat(item->itemFunction, &mapped);
    if (mapped) return c;
    if (looksLikeMapName(safeName((RootObject*)item))) return CAT_MAP;
    Cat sc;
    if (sectionToCat(toLower(item->inventorySection), &sc)) return sc;
    return CAT_OTHER;
}

// Full classifier for the block path, which has a concrete Item*.
static Cat classifyItem(Item* item)
{
    if (!item) return CAT_OTHER;
    if (looksLikeMapName(safeName((RootObject*)item))) return CAT_MAP;
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
    std::set<std::string> stockNames;
    std::set<GameData*> seenVendorLists;
    std::set<std::string> vendorListNames;
    std::set<std::string> vendorListIds;
    bool           hasGeneralTradeAllowList;
    bool           isTradeShop;
    bool           sellsRobotics;
    bool           isWeaponsOnlyShop;
    bool           sellsArmorOrClothing;
    bool           isBar;
    int            liveStockItems;
    int            templateStockRefs;

    ShopContext() : shop(NULL), archetype("_default"), foundVendorList(false), haveStock(false),
        hasGeneralTradeAllowList(false), isTradeShop(false), sellsRobotics(false),
        isWeaponsOnlyShop(false), sellsArmorOrClothing(false), isBar(false),
        liveStockItems(0), templateStockRefs(0) {}
    void clear()
    {
        shop = NULL; archetype = "_default"; foundVendorList = false; haveStock = false;
        stockCats.clear(); stockNames.clear(); seenVendorLists.clear(); vendorListNames.clear(); vendorListIds.clear();
        hasGeneralTradeAllowList = false; isTradeShop = false; sellsRobotics = false;
        isWeaponsOnlyShop = false; sellsArmorOrClothing = false; isBar = false;
        liveStockItems = 0; templateStockRefs = 0;
    }
};

static ShopContext gCtx;

static bool readable(const void* p, size_t n);
static bool isGeneralTradeAllowName(const std::string& normalisedName);

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

static bool nameInList(const std::string& normalisedName, const char* const* names, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (normalisedName == names[i])
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

static bool setContainsSubstring(const std::set<std::string>& values, const char* needle)
{
    for (std::set<std::string>::const_iterator it = values.begin(); it != values.end(); ++it)
        if (it->find(needle) != std::string::npos)
            return true;
    return false;
}

// Identifies vendor lists that define goods general traders should accept even
// when those items are absent from the individual shop's vendor list.
static bool isGeneralTradeAllowList(GameData* vendor)
{
    if (!vendor) return false;
    std::string name = normaliseName(vendor->name);
    std::string id = toLower(vendor->stringID);
    return name == "all trade goods" || name == "trade goods" || name == "all items reference" ||
        id == "10-traders refuse irrelevant items.mod" ||
        id == "1013-gamedata.base" || id == "1386-gamedata.base";
}

// Allows stores to buy obvious inputs for the things they sell, plus the
// mod-owned force-allow list for general trade stores.
static bool isShopSpecificAcceptedGood(const std::string& itemName, Cat cat)
{
    std::string nn = normaliseName(itemName);
    if (nn.empty()) return false;

    static const char* const roboticsInputs[] = {
        "skeleton muscle", "motor", "robotics component", "robotics components",
        "electrical component", "electrical components", "steel bar", "steel bars",
        "skeleton repair kit", "skeleton repair kits", "skeleton eye", "press",
        "power core", "generator core", "gear", "gears", "cpu unit", "capacitor", "capacitors"
    };
    static const char* const weaponInputs[] = {
        "iron plates", "iron plate", "iron ore", "fabrics", "fabric", "steel bars", "steel bar"
    };
    static const char* const armourInputs[] = {
        "fabric", "fabrics", "iron plates", "iron plate", "iron ore",
        "steel bars", "steel bar", "leather", "armor plating", "armour plating"
    };
    static const char* const barGoods[] = {
        "grog", "sake", "cactus rum", "bloodrum", "hashish"
    };

    if (gCtx.sellsRobotics &&
        nameInList(nn, roboticsInputs, sizeof(roboticsInputs) / sizeof(roboticsInputs[0])))
        return true;
    if (gCtx.isWeaponsOnlyShop &&
        nameInList(nn, weaponInputs, sizeof(weaponInputs) / sizeof(weaponInputs[0])))
        return true;
    if (gCtx.sellsArmorOrClothing &&
        nameInList(nn, armourInputs, sizeof(armourInputs) / sizeof(armourInputs[0])))
        return true;
    if (gCtx.isBar &&
        (cat == CAT_FOOD || cat == CAT_BOOZE || cat == CAT_WATER ||
         nameInList(nn, barGoods, sizeof(barGoods) / sizeof(barGoods[0]))))
        return true;
    if (gCtx.isTradeShop &&
        (cat == CAT_TRADEGOODS || isGeneralTradeAllowName(nn)))
        return true;
    return false;
}

// Robot limb shops are inconsistent in FCS naming, so infer them from either
// explicit categories or distinctive stock/list names.
static bool stockLooksLikeRobotics(const ShopContext& ctx)
{
    return ctx.stockCats.count(CAT_ROBOTICS) > 0 ||
        ctx.archetype == "robotics" ||
        setContainsSubstring(ctx.vendorListNames, "robot") ||
        setContainsSubstring(ctx.vendorListNames, "skeleton") ||
        setContainsSubstring(ctx.vendorListNames, "limb") ||
        setContainsSubstring(ctx.vendorListIds, "robot") ||
        setContainsSubstring(ctx.vendorListIds, "skeleton") ||
        setContainsSubstring(ctx.stockNames, "robot") ||
        setContainsSubstring(ctx.stockNames, "skeleton") ||
        setContainsSubstring(ctx.stockNames, "repair kit") ||
        setContainsSubstring(ctx.stockNames, "klr series") ||
        setContainsSubstring(ctx.stockNames, "economy arm") ||
        setContainsSubstring(ctx.stockNames, "economy leg") ||
        setContainsSubstring(ctx.stockNames, "industrial lifter arm") ||
        setContainsSubstring(ctx.stockNames, "steady arm") ||
        setContainsSubstring(ctx.stockNames, "thief") ||
        setContainsSubstring(ctx.stockNames, "scout leg");
}

struct VendorStockList { const char* name; Cat cat; };
static const VendorStockList kVendorStockLists[] = {
    { "crossbows", CAT_RANGED }, { "ammo", CAT_AMMO }, { "ammunition", CAT_AMMO }, { "bolts", CAT_AMMO },
    { "weapons", CAT_WEAPON }, { "armour", CAT_ARMOUR }, { "armor", CAT_ARMOUR },
    { "containers", CAT_BACKPACK }, { "backpacks", CAT_BACKPACK }, { "blueprints", CAT_BLUEPRINT },
    { "maps", CAT_MAP }, { "map", CAT_MAP }, { "medical", CAT_MEDICAL }, { "robotics", CAT_ROBOTICS }, { "food", CAT_FOOD },
    { "building materials", CAT_BUILDMATS }, { "raw materials", CAT_RAWMATS },
    { "trade goods", CAT_TRADEGOODS }, { "items", CAT_OTHER },
};

static Cat gameDataToCat(GameData* data, Cat hint)
{
    if (hint != CAT_OTHER) return hint;
    if (!data) return CAT_OTHER;
    switch (data->type)
    {
    case WEAPON: return CAT_WEAPON;
    case CROSSBOW: return CAT_RANGED;
    case ARMOUR: return CAT_ARMOUR;
    case CONTAINER: return CAT_BACKPACK;
    case BLUEPRINT: return CAT_BLUEPRINT;
    case ARTIFACTS: return CAT_ARTIFACTS;
    case MAP_ITEM: return CAT_MAP;
    default: break;
    }

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

static std::set<std::string> gGeneralTradeAllowNames;
static bool gGeneralTradeAllowListLoaded = false;

// Adds exact item names from a vendor list; FCS stores many trade goods as
// generic ITEM data, so category alone is not enough for this allow-list.
static void addGeneralTradeAllowedItemsFrom(GameData* vendor)
{
    if (!vendor) return;
    for (GameDataReferenceMap::const_iterator it = vendor->objectReferences.begin();
         it != vendor->objectReferences.end(); ++it)
    {
        const Ogre::vector<GameDataReference>::type& refs = it->second;
        uint32_t scanned = refs.size() < 240 ? (uint32_t)refs.size() : 240;
        for (uint32_t i = 0; i < scanned; ++i)
        {
            GameData* itemData = refs[i].ptr;
            if (!itemData || !gameDataTypeCanBeShopStock(itemData->type)) continue;
            std::string nm = normaliseName(itemData->name);
            if (!nm.empty()) gGeneralTradeAllowNames.insert(nm);
        }
    }
}

// Loads the mod-owned force-allow list plus vanilla trade-good lists once.
static void loadGeneralTradeAllowList()
{
    if (gGeneralTradeAllowListLoaded) return;
    gGeneralTradeAllowListLoaded = true;

    if (!ou)
    {
        if (cfg::debug) DebugLog("[TRII][trade_allow] GameWorld unavailable; force-allow items not loaded");
        return;
    }

    static const char* const allowListIds[] = {
        "10-Traders Refuse Irrelevant Items.mod", // mod-owned general-trader force-allow list
        "1013-gamedata.base", // all trade goods
        "1386-gamedata.base"  // trade goods
    };

    for (size_t i = 0; i < sizeof(allowListIds) / sizeof(allowListIds[0]); ++i)
    {
        GameData* vendor = ou->gamedata.getData(std::string(allowListIds[i]), VENDOR_LIST);
        if (!vendor)
        {
            if (cfg::debug)
            {
                char miss[192];
                _snprintf_s(miss, sizeof(miss), _TRUNCATE,
                    "[TRII][trade_allow] vendor list not found id=%s", allowListIds[i]);
                DebugLog(miss);
            }
            continue;
        }
        addGeneralTradeAllowedItemsFrom(vendor);
    }

    if (cfg::debug)
    {
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][trade_allow] listsLoaded=%d items=%d",
            gGeneralTradeAllowNames.empty() ? 0 : 1,
            (int)gGeneralTradeAllowNames.size());
        DebugLog(buf);
    }
}

static bool isGeneralTradeAllowName(const std::string& normalisedName)
{
    loadGeneralTradeAllowList();
    return gGeneralTradeAllowNames.find(normalisedName) != gGeneralTradeAllowNames.end();
}

// Scan the shop's live inventory into a category + normalized-name profile.
static void buildStockProfile(Inventory* inv, ShopContext& ctx)
{
    if (!inv) return;
    const lektor<Item*>& items = inv->getAllItems();
    uint32_t n = items.size();
    if (n == 0) return;
    ctx.liveStockItems += (int)n;
    uint32_t scanned = n < 120 ? n : 120;
    for (uint32_t i = 0; i < scanned; ++i)
    {
        Item* it = items[i];
        if (!it) continue;
        ctx.stockCats.insert(classifyItem(it));
        std::string nm = normaliseName(safeName(it));
        if (!nm.empty()) ctx.stockNames.insert(nm);
    }
    ctx.haveStock = !ctx.stockCats.empty();
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
    if (!vendor->stringID.empty()) ctx.vendorListIds.insert(vendor->stringID);

    if (cfg::scanAllVendorRefs)
    {
        bool generalTradeList = isGeneralTradeAllowList(vendor);
        for (GameDataReferenceMap::const_iterator it = vendor->objectReferences.begin();
             it != vendor->objectReferences.end(); ++it)
        {
            Cat hint = vendorRefListHint(it->first);
            if (generalTradeList && hint == CAT_OTHER)
                hint = CAT_TRADEGOODS;
            const Ogre::vector<GameDataReference>::type& refs = it->second;
            uint32_t scanned = refs.size() < 120 ? (uint32_t)refs.size() : 120;
            for (uint32_t j = 0; j < scanned; ++j)
                addStockGameData(vendor, refs[j].ptr, hint, it->first, (int)j, ctx);
        }
    }
    else
    {
        for (size_t i = 0; i < sizeof(kVendorStockLists) / sizeof(kVendorStockLists[0]); ++i)
        {
            const Ogre::vector<GameDataReference>::type* refs =
                vendor->getReferenceListIfExists(kVendorStockLists[i].name);
            if (!refs || refs->empty()) continue;
            Cat hint = kVendorStockLists[i].cat;
            if (ctx.hasGeneralTradeAllowList && hint == CAT_OTHER)
                hint = CAT_TRADEGOODS;
            if (hint != CAT_OTHER) ctx.stockCats.insert(hint);
            uint32_t scanned = refs->size() < 80 ? (uint32_t)refs->size() : 80;
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

static void deriveShopTraits(ShopContext& ctx)
{
    ctx.sellsRobotics = stockLooksLikeRobotics(ctx);
    ctx.isWeaponsOnlyShop = stockHasOnlyWeaponGoods(ctx.stockCats);
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
            "[TRII][shop] name=%s archetype=%s foundVendorList=%d haveStock=%d stockCats=%d stockCatList=%s stockNames=%d stockNameSample=%s vendorLists=%d vendorListNames=%s tradeAllowList=%d tradeShop=%d robotics=%d weaponOnly=%d armour=%d bar=%d liveItems=%d templateRefs=%d",
            safeName(shop).c_str(), gCtx.archetype.c_str(), (int)gCtx.foundVendorList, (int)gCtx.haveStock,
            (int)gCtx.stockCats.size(), cats.c_str(), (int)gCtx.stockNames.size(),
            stockNames.c_str(), (int)gCtx.vendorListNames.size(), vendorLists.c_str(),
            (int)gCtx.hasGeneralTradeAllowList, (int)gCtx.isTradeShop, (int)gCtx.sellsRobotics,
            (int)gCtx.isWeaponsOnlyShop, (int)gCtx.sellsArmorOrClothing, (int)gCtx.isBar,
            gCtx.liveStockItems, gCtx.templateStockRefs);
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

// Some shop stock categories imply closely-related goods. Crossbow shops may
// expose only their weapons as stock, but bolts are still relevant to them.
static bool stockAcceptsRelated(Cat cat)
{
    if (cat == CAT_AMMO && gCtx.stockCats.count(CAT_RANGED)) return true;
    return false;
}

// The shop's real stock expands acceptance; otherwise fall back to the rule.
static Disp dispositionFor(Cat cat, const std::string& itemName, double* reducedMult)
{
    const Rule& rule = ruleFor(gCtx.archetype);
    if (cfg::useStockAcceptance && gCtx.haveStock)
    {
        if (cat != CAT_OTHER && (gCtx.stockCats.count(cat) || stockAcceptsRelated(cat))) return DISP_FULL;
        std::string nn = normaliseName(itemName);
        if (!nn.empty() && gCtx.stockNames.count(nn)) return DISP_FULL;
        if (isShopSpecificAcceptedGood(itemName, cat)) return DISP_FULL;
        if (cat == CAT_FOOD)
        {
            if (reducedMult) *reducedMult = cfg::offListFoodMult;
            return DISP_REDUCED;
        }
        // stock is known and the item matched neither category nor name:
        // still honour a reduced rule (e.g. food), else refuse.
        std::map<Cat, double>::const_iterator it = rule.reduced.find(cat);
        if (it != rule.reduced.end()) { if (reducedMult) *reducedMult = it->second; return DISP_REDUCED; }
        // With a vendor list present, unknown/off-list items should not slip
        // through just because the classifier could not name their category.
        return DISP_REFUSE;
    }
    if (cfg::useStockAcceptance && cfg::acceptAllWhenNoVendorList && !gCtx.foundVendorList)
        return DISP_FULL;
    return baseDisposition(rule, cat, reducedMult);
}

// ---------------------------------------------------------------------
//  HOOK 1: value cue on the player's sell side
//  InventoryItemBase::getValueSingle(bool isPlayer) is virtual, so hook the
//  non-virtual body via &InventoryItemBase::_NV_getValueSingle. Subclasses may
//  keep their own value path, so hard refusal must still live in add/placement.
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
//  FAULT DIAGNOSTICS
//  SEH turns a bad dereference into RE'd game memory into a logged reason +
//  safe default, instead of crashing Kenshi. This filter records the fault
//  site, exception code and address ONCE per site (so it can't spam a
//  per-frame hook), then runs the __except body (which returns the default).
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

typedef int (*GetValueSingle_t)(InventoryItemBase*, bool);
static GetValueSingle_t GetValueSingle_orig = NULL;

static bool gDbgValueFire = false;
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

static int cancelTraderSellMultiplier(int desiredValue, float traderMult)
{
    if (!cfg::payFullLocalPrice) return desiredValue;
    if (desiredValue <= 0) return desiredValue;
    if (traderMult < 0.01f || traderMult > 10.0f) return desiredValue;
    if (std::fabs(traderMult - 1.0f) < 0.001f) return desiredValue;
    return (int)std::ceil((double)desiredValue / (double)traderMult);
}

static void logValueDecision(InventoryItemBase* item, Cat cat, Disp disp,
                             int base, int avg, int returned, float traderMult)
{
    if (!cfg::debug) return;
    if (gSeenValueProfiles.size() >= 80) return;
    std::string section = item ? item->inventorySection : std::string();
    std::string itemName = item ? safeName((RootObject*)item) : std::string();
    std::string key = gCtx.archetype + "|" + catLabel(cat) + "|" +
        dispLabel(disp) + "|" + section + "|" + itemName;
    if (gSeenValueProfiles.find(key) != gSeenValueProfiles.end()) return;
    gSeenValueProfiles.insert(key);

    char buf[384];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[TRII][item] shop=%s archetype=%s item=%s section=%s fn=%d cat=%s disp=%s base=%d avg=%d ret=%d traderMult=%.3f stockCats=%d stockNames=%d",
        safeName(gCtx.shop).c_str(), gCtx.archetype.c_str(), itemName.c_str(), section.c_str(),
        item ? (int)item->itemFunction : -1, catLabel(cat), dispLabel(disp),
        base, avg, returned, traderMult, (int)gCtx.stockCats.size(), (int)gCtx.stockNames.size());
    DebugLog(buf);
}

// All the C++ logic (std::string/std::set locals) lives here so the SEH shell
// below stays free of objects that need unwinding (MSVC C2712).
static int GetValueSingle_impl(InventoryItemBase* self, bool isPlayer, int base)
{
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
    Disp disp = dispositionFor(cat, safeName((RootObject*)self), &mult);
    float traderMult = safeTraderPriceMultiplier();

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
    if (disp == DISP_REDUCED)
    {
        int avg = self->getAvgPrice();
        int desired = (int)std::floor(avg * mult);
        int ret = cancelTraderSellMultiplier(desired, traderMult);
        logValueDecision(self, cat, disp, base, avg, ret, traderMult);
        return ret;
    }
    if (disp == DISP_FULL && cfg::payFullLocalPrice)
    {
        int avg = self->getAvgPrice();
        int ret = cancelTraderSellMultiplier(avg, traderMult);
        logValueDecision(self, cat, disp, base, avg, ret, traderMult);
        return ret;
    }
    logValueDecision(self, cat, disp, base, -1, base, traderMult);
    return base;
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
    bool sourceIsPlayer = itemBelongsToPlayerSide(item, sourceOwner) ||
                          activeTradeHadPlayerItem(item) ||
                          selfOwnedContainerSale ||
                          ownerlessPlayerSale;

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
        disp = dispositionFor(cat, safeName(item), NULL);
    }
    if (catOut) *catOut = cat;
    if (dispOut) *dispOut = disp;

    return quantity > 0 && destIsShop && sourceIsPlayer;
}

static void refusalFeedbackGuarded(Character* speaker);

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

    int protectedContainerItems = readable(itemToAdd, 8) ? backpackContainerContentCount(itemToAdd) : 0;
    bool protectContainerContents = playerSale && protectedContainerItems > 0;
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
            refusalFeedbackGuarded(speaker);
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
            refusalFeedbackGuarded(speaker);
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

// InventorySection::_addItem can be reached after the mouse already picked up
// the item. Returning early blocks the shop placement, but without this restore
// the item can vanish from the visible player inventory until the engine catches
// up, so refused drag/drop sales are explicitly handed back.
static bool restoreRefusedItemToPlayerImpl(Item* item, int quantity)
{
    if (!cfg::restoreRefusedDragDropItems) return false;
    if (!readable(item, 8)) return false;
    if (!readable(gTrade.player, 8)) return false;

    Inventory* playerInv = gTrade.player->getInventory();
    if (!readable(playerInv, 8)) return false;

    RootObject* currentOwner = itemInventoryOwner(item);
    if (sameTradeObject(currentOwner, gTrade.player))
        return true;

    bool ret = false;
    if (InventoryAddItem_orig)
        ret = InventoryAddItem_orig(playerInv, item, quantity > 0 ? quantity : 1, false, false);
    else
        ret = playerInv->addItem(item, quantity > 0 ? quantity : 1, false, false);

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
            refusalFeedbackGuarded(speaker);
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
            refusalFeedbackGuarded(speaker);
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
            refusalFeedbackGuarded(speaker);
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

static bool refusalModeIs(const char* mode)
{
    return std::string(cfg::refusalMode) == mode;
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
        case 0: return "Rejected. Merchant criteria not met.";
        case 1: return "Repeated rejection acknowledged.";
        case 2: return "Please select a compatible item.";
        default: return "Still incompatible.";
        }
    }
    return genericRefusalLineForCount(count);
}

static void speakRefusal(Character* speaker)
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
    const char* line = raceRefusalLineForCount(speakerRace, targetRace, count);
    speaker->sayALine(std::string(line), true);
    if (cfg::debug)
    {
        std::string speakerRaceData = raceDataSummary(speaker);
        std::string targetRaceData = raceDataSummary(target);
        char buf[960];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[TRII][refusal_speech] speaker=%s target=%s speakerRace=%s targetRace=%s count=%d line=%s speakerData={%s} targetData={%s}",
            objectSummary((RootObject*)speaker).c_str(), objectSummary((RootObject*)target).c_str(),
            raceGroupLabel(speakerRace), raceGroupLabel(targetRace), count, line,
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

// Trade-window open is our stable reset point for per-trade state.
void (*_showTradeWindow_orig)(ForgottenGUI* thisptr, RootObject* a, RootObject* b, TradeWindowType tradeType);
void _showTradeWindow_hook(ForgottenGUI* thisptr, RootObject* a, RootObject* b, TradeWindowType tradeType)
{
    logTradeWindowOpen("pre", thisptr, a, b, tradeType);
    _showTradeWindow_orig(thisptr, a, b, tradeType);
    updateActiveTradeContext(thisptr, a, b, tradeType);
    logTradeWindowOpen("post", thisptr, a, b, tradeType);
}

static void refusalFeedbackGuarded(Character* speaker)
{
    __try
    {
        speakRefusal(speaker);
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

    if (!playerSale || (disp != DISP_REFUSE && protectedContainerItems <= 0)) return false;

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
            refusalFeedbackGuarded(speaker);
            return false;
        }
    }
    return PlaceItemFromMouse_orig(self, sectionName, mousePos);
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
        intptr_t addr = KenshiLib::GetRealAddress(&ForgottenGUI::_showTradeWindow);
        bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(addr, &_showTradeWindow_hook, &_showTradeWindow_orig));
        logAddr("ForgottenGUI::_showTradeWindow", addr, ok);
        if (!ok) ErrorLog("TradersRefuse: could not hook ForgottenGUI::_showTradeWindow");
    }

    DebugLog("TradersRefuse: native plugin loaded");
}
