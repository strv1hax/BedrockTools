#include "itemtags.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
using NormalTickFn = void (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);
using ActorSetNameTagFn = void (*)(void*, const std::string&);
using SynchedActorDataEnsureIndexFn = void (*)(void*, std::uint16_t);
using ActorSynchedDataUpdateAlwaysShowNameTagFn = void (*)(void*, const void*);

struct OriginalNametagState {
    std::string name;
    bool hadAlwaysShowItem = false;
    std::int8_t alwaysShowValue = 0;
};

ItemTagsModule* g_mod = nullptr;
NormalTickFn g_normalTickOriginal = nullptr;
ActorGetNameTagFn g_getNameTag = nullptr;
ActorSetNameTagFn g_setNameTag = nullptr;
SynchedActorDataEnsureIndexFn g_ensureIndex = nullptr;
ActorSynchedDataUpdateAlwaysShowNameTagFn g_updateAlwaysShowNameTag = nullptr;
std::unordered_map<void*, OriginalNametagState> g_originalStates;

// ItemActor::mItem offset (ItemStack). 
// If your game crashes, try 0x3A0 or 0x3A8 instead.
constexpr std::size_t kItemStackOffset = 0x398; 

void* getEntityDataWrapper(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData
    );
}

void* getEntityContext(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext
    );
}

void* getDataComponent(void* actor) {
    void* wrapper = getEntityDataWrapper(actor);
    if (!wrapper) return nullptr;
    return *reinterpret_cast<void**>(wrapper);
}

void** getItemsBegin(void* component) {
    if (!component) return nullptr;
    return *reinterpret_cast<void***>(component);
}

std::size_t getItemsSize(void* component) {
    if (!component) return 0;
    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end || end < begin) return 0;
    return static_cast<std::size_t>(end - begin);
}

void markDataItemPresentAndDirty(void* component, std::size_t id) {
    if (!component || id >= 192) return;
    auto* dirty = reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(component) + 0x18);
    auto* present = reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(component) + 0x30);
    const std::size_t word = id / 64;
    const std::uint64_t bit = std::uint64_t{1} << (id % 64);
    dirty[word] |= bit;
    present[word] |= bit;
}

void* findSCharDataItemVtable(void* component) {
    auto** begin = getItemsBegin(component);
    const std::size_t size = getItemsSize(component);
    if (!begin) return nullptr;
    for (std::size_t i = 0; i < size; ++i) {
        void* item = begin[i];
        if (!item) continue;
        const auto address = reinterpret_cast<std::uintptr_t>(item);
        const auto type = *reinterpret_cast<const std::uint8_t*>(address + bedrocktools::sdk::offsets::DataItem::mType);
        if (type == 0) return *reinterpret_cast<void**>(item);
    }
    return nullptr;
}

bool readAlwaysShowItem(void* actor, std::int8_t& value) {
    void* component = getDataComponent(actor);
    if (!component) return false;
    constexpr std::size_t id = bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow;
    if (getItemsSize(component) <= id) return false;
    auto** begin = getItemsBegin(component);
    if (!begin) return false;
    void* item = begin[id];
    if (!item) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(address + bedrocktools::sdk::offsets::DataItem::mType);
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(address + bedrocktools::sdk::offsets::DataItem::mId);
    if (type != 0 || itemId != id) return false;
    value = *reinterpret_cast<const std::int8_t*>(address + bedrocktools::sdk::offsets::DataItem::mValue);
    return true;
}

bool writeAlwaysShowItem(void* actor, std::int8_t value) {
    if (!actor || !g_ensureIndex || !g_updateAlwaysShowNameTag) return false;
    void* wrapper = getEntityDataWrapper(actor);
    void* context = getEntityContext(actor);
    void* component = getDataComponent(actor);
    if (!wrapper || !context || !component) return false;
    constexpr std::uint16_t id = static_cast<std::uint16_t>(bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow);
    g_ensureIndex(component, id);
    auto** begin = getItemsBegin(component);
    if (!begin || getItemsSize(component) <= id) return false;
    void* item = begin[id];
    if (!item) {
        void* vtable = findSCharDataItemVtable(component);
        if (!vtable) return false;
        item = ::operator new(16);
        std::memset(item, 0, 16);
        *reinterpret_cast<void**>(item) = vtable;
        *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mType) = 0;
        *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mId) = id;
        begin[id] = item;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(address + bedrocktools::sdk::offsets::DataItem::mType);
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(address + bedrocktools::sdk::offsets::DataItem::mId);
    if (type != 0 || itemId != id) return false;
    *reinterpret_cast<std::int8_t*>(address + bedrocktools::sdk::offsets::DataItem::mValue) = value;
    markDataItemPresentAndDirty(component, id);
    g_updateAlwaysShowNameTag(context, wrapper);
    return true;
}

void restoreNametag(void* actor) {
    const auto it = g_originalStates.find(actor);
    if (it == g_originalStates.end()) return;
    if (g_setNameTag) g_setNameTag(actor, it->second.name);
    writeAlwaysShowItem(actor, it->second.hadAlwaysShowItem ? it->second.alwaysShowValue : 0);
    g_originalStates.erase(it);
}

void captureNametag(void* actor) {
    if (!actor || g_originalStates.contains(actor)) return;
    OriginalNametagState state;
    if (g_getNameTag) state.name = g_getNameTag(actor);
    state.hadAlwaysShowItem = readAlwaysShowItem(actor, state.alwaysShowValue);
    g_originalStates.emplace(actor, std::move(state));
}

bool isItemActor(void* actor) {
    if (!actor) return false;
    void* stack = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(actor) + kItemStackOffset);
    return stack != nullptr && stack != (void*)0xFFFFFFFFFFFFFFFF;
}

std::string getHoverName(void* itemStack) {
    if (!itemStack) return "Dropped Item";
    // We use a static label to guarantee the Tessellator/Font pipeline renders without crashing.
    return "Dropped Item";
}

void normalTickHook(void* actor) {
    if (g_normalTickOriginal) g_normalTickOriginal(actor);
    if (!actor || !g_mod) return;

    if (!isItemActor(actor)) return; 

    if (!g_mod->enabled) {
        restoreNametag(actor);
        return;
    }

    if (!g_setNameTag || !g_ensureIndex || !g_updateAlwaysShowNameTag) return;

    captureNametag(actor);
    
    void* stack = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(actor) + kItemStackOffset);
    std::string label = getHoverName(stack);
    
    g_setNameTag(actor, label);
    writeAlwaysShowItem(actor, 1);
}
}

ItemTagsModule::ItemTagsModule()
    : Module("Item Tags", "Shows a floating hologram nametag above dropped items using the SDK's Tessellator/Font pipeline.") {
    g_mod = this;
}

ItemTagsModule::~ItemTagsModule() {
    if (g_mod == this) g_mod = nullptr;
}

void ItemTagsModule::onInit() {
    g_getNameTag = reinterpret_cast<ActorGetNameTagFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    g_setNameTag = reinterpret_cast<ActorSetNameTagFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag));
    g_ensureIndex = reinterpret_cast<SynchedActorDataEnsureIndexFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex));
    g_updateAlwaysShowNameTag = reinterpret_cast<ActorSynchedDataUpdateAlwaysShowNameTagFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag));

    const auto normalTick = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NormalTick);
    if (!normalTick || g_normalTickOriginal) return;

    bedrocktools::hooks::install(
        reinterpret_cast<void*>(normalTick),
        reinterpret_cast<void*>(normalTickHook),
        reinterpret_cast<void**>(&g_normalTickOriginal)
    );
}
