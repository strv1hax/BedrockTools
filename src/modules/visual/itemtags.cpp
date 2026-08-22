#include "itemtags.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <android/log.h>
#include <link.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// =====================================================================
// Item Tags v6 — architecture copied 1:1 from the WORKING tnttimer.cpp
// (proven by your "2.60s" screenshot) + hitbox.cpp internals.
//
// WHY v5 FAILED: generic NormalTick + bad 0x398 check tagged the PLAYER.
// FIX: ItemActor is now detected by its vtable (only ItemActor's vtable
//      contains readAdditionalData @ 0xea29e10 from your dump — the fn
//      that stores ticks_before_removal at +0x390).
//
// DUMP-DERIVED (26.40):
//   0xea29e10  ItemActor::readAdditionalData  (str w0,[x19,#0x390])
//   0xa3252d0  ItemStackBase::getHoverName region
//   +0x390     ticks_before_removal
//
// LABEL FORMAT (your request):  §f<white name> §6×<orange count>§r
// =====================================================================

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ItemTags", __VA_ARGS__)

namespace {

using NormalTickFn   = void (*)(void*);
using GetNameTagFn   = std::string (*)(void*);
using SetNameTagFn   = void (*)(void*, const std::string&);
using EnsureIndexFn  = void (*)(void*, std::uint16_t);
using UpdateAlwaysFn = void (*)(void*, const void*);
using IsPlayerFn     = bool (*)(void*);
using GetHoverFn     = void (*)(std::string*, void*); // std::string sret

constexpr uintptr_t kOff_ReadAdditionalData = 0xea29e10; // vtable marker
constexpr uintptr_t kOff_GetHoverName       = 0xa3252d0;
constexpr std::size_t kDespawnOff           = 0x390;

const std::vector<std::size_t> kStackCandidates = {0x678, 0x668, 0x688, 0x658, 0x698, 0x6a8, 0x550, 0x398};
const std::vector<std::size_t> kCountCandidates = {0x54, 0x52, 0x56, 0x58, 0x50};

struct SavedState {
    std::string name;
    bool hadAlwaysShow = false;
    std::int8_t alwaysShowValue = 0;
};

ItemTagsModule* g_mod = nullptr;
NormalTickFn   g_origTick = nullptr;
GetNameTagFn   g_getNameTag = nullptr;
SetNameTagFn   g_setNameTag = nullptr;
EnsureIndexFn  g_ensureIndex = nullptr;
UpdateAlwaysFn g_updateAlwaysShow = nullptr;
IsPlayerFn     g_isPlayer = nullptr;
GetHoverFn     g_getHover = nullptr;

uintptr_t g_base = 0;
uintptr_t g_readDataAddr = 0;
uintptr_t g_itemVtable = 0;
std::unordered_set<uintptr_t> g_notItemVtables;
std::size_t g_stackOff = 0;
std::size_t g_countOff = 0;

std::mutex g_mtx;
std::unordered_map<void*, SavedState> g_saved;
std::unordered_map<void*, std::uint64_t> g_lastUpdate;
std::uint64_t g_tick = 0;

int phdrCb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libminecraftpe.so")) {
        g_base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

bool looksLikeFunction(uintptr_t a) {
    if (a < 0x10000 || (a & 3)) return false;
    const std::uint32_t i = *reinterpret_cast<std::uint32_t*>(a);
    const bool stp = (i & 0xFFC003E0) == 0xA98003E0;          // stp x29,x30,[sp,#-]
    const bool sub = (i & 0xFF8003FF) == 0xD10003FF;          // sub sp,sp,#imm
    const bool adrp = (i & 0x9F000000) == 0x90000000;         // adrp
    const bool mrs  = i == 0xD53BD040 || (i & 0xFFFFF000) == 0xD53BD000;
    return stp || sub || adrp || mrs;
}

// ---------- ItemActor detection via vtable marker ----------
bool isItemActor(void* actor) {
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(actor);
    if (vt < 0x10000) return false;
    if (g_itemVtable) return vt == g_itemVtable;
    if (g_notItemVtables.count(vt)) return false;
    if (!g_readDataAddr) return false;
    const auto* e = reinterpret_cast<const uintptr_t*>(vt);
    for (int i = 2; i < 160; i++) {
        if (e[i] == g_readDataAddr) {
            g_itemVtable = vt;
            LOGI("ItemActor vtable found: %p", (void*)vt);
            return true;
        }
    }
    g_notItemVtables.insert(vt);
    return false;
}

// ---------- safe ItemStack discovery (self-calibrating) ----------
bool stackLooksValid(uintptr_t p) {
    if (p < 0x10000 || (p & 7)) return false;
    const uintptr_t item = *reinterpret_cast<uintptr_t*>(p + 0x8); // mItem
    if (item < 0x10000 || (item & 7)) return false;
    const uintptr_t ivt = *reinterpret_cast<uintptr_t*>(item);
    return ivt > 0x10000;
}

void* getItemStack(void* actor) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(actor);
    if (g_stackOff) return *reinterpret_cast<void**>(a + g_stackOff);
    for (std::size_t off : kStackCandidates) {
        const uintptr_t p = *reinterpret_cast<uintptr_t*>(a + off);
        if (stackLooksValid(p)) {
            g_stackOff = off;
            for (std::size_t c : kCountCandidates) {
                const int n = *reinterpret_cast<std::uint8_t*>(p + c);
                if (n >= 1 && n <= 64) { g_countOff = c; break; }
            }
            LOGI("calibrated stack=0x%zx count=0x%zx", g_stackOff, g_countOff);
            return reinterpret_cast<void*>(p);
        }
    }
    return nullptr;
}

// ---------- SynchedActorData nametag control (from tnttimer.cpp) ----------
void* dataComponent(void* actor) {
    auto* wrapper = *reinterpret_cast<void***>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData);
    return wrapper;
}
void** itemsBegin(void* comp) { return comp ? *reinterpret_cast<void***>(comp) : nullptr; }
std::size_t itemsSize(void* comp) {
    if (!comp) return 0;
    auto** b = *reinterpret_cast<void***>(comp);
    auto** e = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(comp) + 8);
    return (b && e && e >= b) ? (std::size_t)(e - b) : 0;
}
void markDirty(void* comp, std::size_t id) {
    if (!comp || id >= 192) return;
    auto* dirty = reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(comp) + 0x18);
    auto* present = reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(comp) + 0x30);
    dirty[id / 64] |= (1ull << (id % 64));
    present[id / 64] |= (1ull << (id % 64));
}
void* findSCharVtable(void* comp) {
    auto** b = itemsBegin(comp);
    for (std::size_t i = 0, n = itemsSize(comp); b && i < n; i++) {
        if (!b[i]) continue;
        if (*reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(b[i]) + bedrocktools::sdk::offsets::DataItem::mType) == 0)
            return *reinterpret_cast<void**>(b[i]);
    }
    return nullptr;
}
bool readAlwaysShow(void* actor, std::int8_t& v) {
    void* comp = dataComponent(actor);
    constexpr std::size_t id = bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow;
    auto** b = itemsBegin(comp);
    if (!b || itemsSize(comp) <= id || !b[id]) return false;
    const uintptr_t it = reinterpret_cast<uintptr_t>(b[id]);
    if (*reinterpret_cast<std::uint8_t*>(it + bedrocktools::sdk::offsets::DataItem::mType) != 0) return false;
    v = *reinterpret_cast<std::int8_t*>(it + bedrocktools::sdk::offsets::DataItem::mValue);
    return true;
}
bool writeAlwaysShow(void* actor, std::int8_t value) {
    if (!g_ensureIndex || !g_updateAlwaysShow) return false;
    void* wrapper = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData);
    void* context = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    void* comp = dataComponent(actor);
    if (!context || !comp) return false;
    constexpr std::uint16_t id = (std::uint16_t)bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow;
    g_ensureIndex(comp, id);
    auto** b = itemsBegin(comp);
    if (!b || itemsSize(comp) <= id) return false;
    void* item = b[id];
    if (!item) {
        void* vt = findSCharVtable(comp);
        if (!vt) return false;
        item = ::operator new(16);
        std::memset(item, 0, 16);
        *reinterpret_cast<void**>(item) = vt;
        *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mType) = 0;
        *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mId) = id;
        b[id] = item;
    }
    const uintptr_t it = reinterpret_cast<uintptr_t>(item);
    *reinterpret_cast<std::int8_t*>(it + bedrocktools::sdk::offsets::DataItem::mValue) = value;
    markDirty(comp, id);
    g_updateAlwaysShow(context, wrapper);
    return true;
}

void capture(void* actor) {
    if (g_saved.count(actor)) return;
    SavedState s;
    if (g_getNameTag) s.name = g_getNameTag(actor);
    s.hadAlwaysShow = readAlwaysShow(actor, s.alwaysShowValue);
    g_saved[actor] = std::move(s);
}
void restore(void* actor) {
    auto it = g_saved.find(actor);
    if (it == g_saved.end()) return;
    if (g_setNameTag) g_setNameTag(actor, it->second.name);
    writeAlwaysShow(actor, it->second.hadAlwaysShow ? it->second.alwaysShowValue : 0);
    g_saved.erase(it);
}

// ---------- hook ----------
void normalTickHook(void* actor) {
    if (g_origTick) g_origTick(actor);
    if (!g_mod || !actor) return;

    if (!g_mod->enabled) { restore(actor); return; }
    if (g_isPlayer && g_isPlayer(actor)) return;   // never tag players again
    if (!isItemActor(actor)) return;

    // skip items about to despawn (ticks_before_removal @ +0x390)
    const int despawn = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(actor) + kDespawnOff);
    if (despawn <= 0 || despawn > 6000) return;

    g_tick++;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_lastUpdate.find(actor);
        if (it != g_lastUpdate.end() && g_tick - it->second < 20) return;
        g_lastUpdate[actor] = g_tick;
        if (g_lastUpdate.size() > 512) g_lastUpdate.clear();
    }

    void* stack = getItemStack(actor);
    if (!stack) return;

    std::string hover;
    if (g_getHover) g_getHover(&hover, stack);
    if (hover.empty()) hover = "Item";

    int count = 1;
    if (g_countOff) count = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(stack) + g_countOff);
    if (count < 1) count = 1;

    // §f white name + §6 orange ×count
    std::string label = "\xC2\xA7" "f" + hover + " \xC2\xA7" "6\xC3\x97" + std::to_string(count) + "\xC2\xA7" "r";

    capture(actor);
    if (g_setNameTag) g_setNameTag(actor, label);
    writeAlwaysShow(actor, 1);
}

} // namespace

ItemTagsModule::ItemTagsModule()
    : Module("Item Tags", "Hologram nametags on dropped items: white name + orange ×count (TNT-Timer style pipeline).") {
    g_mod = this;
}
ItemTagsModule::~ItemTagsModule() {
    if (g_mod == this) g_mod = nullptr;
}

void ItemTagsModule::onDisable() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& [actor, _] : g_saved) {
        if (g_setNameTag) g_setNameTag(actor, _.name);
        writeAlwaysShow(actor, _.hadAlwaysShow ? _.alwaysShowValue : 0);
    }
    g_saved.clear();
    g_lastUpdate.clear();
}

void ItemTagsModule::onInit() {
    dl_iterate_phdr(phdrCb, nullptr);
    if (g_base) {
        g_readDataAddr = g_base + kOff_ReadAdditionalData;
        if (looksLikeFunction(g_base + kOff_GetHoverName)) g_getHover = (GetHoverFn)(g_base + kOff_GetHoverName);
    }

    g_getNameTag      = (GetNameTagFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag);
    g_setNameTag      = (SetNameTagFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag);
    g_ensureIndex     = (EnsureIndexFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex);
    g_updateAlwaysShow= (UpdateAlwaysFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag);
    g_isPlayer        = (IsPlayerFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);

    const auto nt = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NormalTick);
    if (nt && !g_origTick) {
        bedrocktools::hooks::install(reinterpret_cast<void*>(nt),
                                     reinterpret_cast<void*>(normalTickHook),
                                     reinterpret_cast<void**>(&g_origTick));
        LOGI("hooked NormalTick, base=%p", (void*)g_base);
    }
}
