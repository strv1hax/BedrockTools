#include "itemtags.hpp"

#include "core/memory/Hooks.hpp"

#include <android/log.h>
#include <link.h>
#include <elf.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// =====================================================================
// Item Tags v4 (BedrockTools 1.4.6 + zaphkiel dump internals)
//
// INTERNALS & TESSELLATOR PIPELINE:
// The hologram in the reference screenshot is drawn by LevelRenderer::renderNames.
// It uses Font::renderText for the text and the Tessellator (Fill::renderRect)
// for the background box. Because the v1.4.6 SDK does not expose raw
// Tessellator/Font APIs for entity overlays, we use Actor::setNameTag to
// trigger that exact internal pipeline.
//
// OFFSETS FROM ZAPHKIEL DUMP (26.40/26.50):
// - 0xea29b70: ldr w2, [x20, #0x390] -> ticks_before_removal (despawn timer)
// - 0xb8121f0: ldur q0, [x22, #0x28] -> ItemStack position copy
// - 0xb9044fc: ItemActor::normalTick function region
// =====================================================================

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ItemTags", __VA_ARGS__)

namespace {

// Version-specific offsets from your dump. Fill any 0x0 via so-diff.
constexpr uintptr_t kOff_normalTick      = 0xb9044fc; // From dump: normalTick region
constexpr uintptr_t kOff_getItemStack    = 0x0;       // Use dynsym
constexpr uintptr_t kOff_getHoverName    = 0xa3252d0; // From dump: hover-name builder
constexpr uintptr_t kOff_getCount        = 0x11b913e0; // From dump: getCount PLT
constexpr uintptr_t kOff_setNameTag      = 0x0;
constexpr uintptr_t kOff_setVisible      = 0x0;
constexpr uintptr_t kOff_setAlwaysShow   = 0x0;
constexpr uintptr_t kOff_ticksBeforeRem  = 0x390;     // From dump: 0xea29b70

using NormalTickFn = void (*)(void*);
using GetStackFn   = void* (*)(void*);
using GetHoverFn   = void (*)(std::string*, void*);
using GetCountFn   = int (*)(void*);
using SetTagFn     = void (*)(void*, const std::string*);
using SetFlagFn    = void (*)(void*, bool);

NormalTickFn g_orig = nullptr;
GetStackFn   g_getStack = nullptr;
GetHoverFn   g_getHover = nullptr;
GetCountFn   g_getCount = nullptr;
SetTagFn     g_setTag = nullptr;
SetFlagFn    g_setVisible = nullptr;
SetFlagFn    g_setShow = nullptr;
ItemTagsModule* g_mod = nullptr;

std::mutex g_mtx;
std::unordered_map<void*, uint64_t> g_seen;
uint64_t g_tick = 0;

uintptr_t g_base = 0;
ElfW(Dyn)* g_dyn = nullptr;

int phdrCb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libminecraftpe.so")) {
        g_base = info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
                g_dyn = (ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        return 1;
    }
    return 0;
}

void* dynsym(const char* name) {
    if (!g_dyn) return nullptr;
    ElfW(Sym)* sym = nullptr;
    const char* str = nullptr;
    uintptr_t n = 0;
    for (ElfW(Dyn)* d = g_dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) sym = (ElfW(Sym)*)d->d_un.d_ptr;
        else if (d->d_tag == DT_STRTAB) str = (const char*)d->d_un.d_ptr;
        else if (d->d_tag == DT_HASH) n = ((const uintptr_t*)d->d_un.d_ptr)[1];
    }
    if (!sym || !str || !n) return nullptr;
    for (uintptr_t i = 0; i < n; i++)
        if (sym[i].st_value && sym[i].st_shndx != SHN_UNDEF &&
            !strcmp(str + sym[i].st_name, name))
            return (void*)(g_base + sym[i].st_value);
    return nullptr;
}

void* resolve(const char* mangled, uintptr_t off) {
    if (void* p = dynsym(mangled)) { LOGI("dynsym %s", mangled); return p; }
    if (off && g_base) { LOGI("offset %s -> %p", mangled, (void*)off); return (void*)(g_base + off); }
    LOGI("MISSING %s", mangled);
    return nullptr;
}

void tickHook(void* self) {
    if (g_orig) g_orig(self);
    if (!g_mod || !g_mod->enabled || !self) return;
    if (!g_getStack || !g_getHover || !g_setTag || !g_setVisible || !g_setShow) return;

    // Prevent flickering when item is about to despawn (ticks_before_removal at +0x390)
    int ticksLeft = *(int*)((uintptr_t)self + kOff_ticksBeforeRem);
    if (ticksLeft < 20) return; 

    g_tick++;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_seen.find(self);
        if (it != g_seen.end() && g_tick - it->second < 20) return;
        g_seen[self] = g_tick;
        if (g_seen.size() > 512) g_seen.clear();
    }

    void* stack = g_getStack(self);
    if (!stack) return;

    std::string label;
    g_getHover(&label, stack);
    if (label.empty()) return;
    if (g_getCount) {
        label += " \xC3\x97"; // ×
        label += std::to_string(g_getCount(stack));
    }

    // Triggers LevelRenderer::renderNames -> Font::renderText + Tessellator::renderRect
    g_setTag(self, &label);
    g_setVisible(self, true);
    g_setShow(self, true);
}

} // namespace

ItemTagsModule::ItemTagsModule()
    : Module("Item Tags",
             "Floating hologram labels (name ×count) above dropped items. Uses internal Font+Tessellator pipeline.") {
    g_mod = this;
}

ItemTagsModule::~ItemTagsModule() {
    if (g_mod == this) g_mod = nullptr;
}

void ItemTagsModule::onInit() {
    dl_iterate_phdr(phdrCb, nullptr);
    LOGI("libminecraftpe base=%p", (void*)g_base);

    g_getStack   = (GetStackFn)resolve("_ZNK9ItemActor11getItemStackEv", kOff_getItemStack);
    g_getCount   = (GetCountFn)resolve("_ZNK13ItemStackBase8getCountEv", kOff_getCount);
    g_getHover   = (GetHoverFn)resolve("_ZNK13ItemStackBase11getHoverNameEv", kOff_getHoverName);
    g_setTag     = (SetTagFn)resolve("_ZN5Actor9setNameTagERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE", kOff_setNameTag);
    g_setVisible = (SetFlagFn)resolve("_ZN5Actor15setNameTagVisibleEb", kOff_setVisible);
    g_setShow    = (SetFlagFn)resolve("_ZN5Actor18setNameTagAlwaysShowEb", kOff_setAlwaysShow);

    // Hook normalTick using the exact offset from your dump (0xb9044fc)
    void* tick = resolve("_ZN9ItemActor10normalTickEv", kOff_normalTick);
    if (tick && !g_orig) {
        bedrocktools::hooks::install(tick, (void*)tickHook, (void**)&g_orig);
        LOGI("normalTick hooked @ %p", tick);
    } else {
        LOGI("normalTick NOT resolved -> check kOff_normalTick");
    }
}
