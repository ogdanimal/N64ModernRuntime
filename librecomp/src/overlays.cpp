#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "recompiler/context.h"
#include "overlays.hpp"
#include "sections.h"

static recomp::overlays::overlay_section_table_data_t sections_info {};
static recomp::overlays::overlays_by_index_t overlays_info {};

static SectionTableEntry* patch_code_sections = nullptr;
size_t num_patch_code_sections = 0;
static std::vector<char> patch_data;

struct LoadedSection {
    int32_t loaded_ram_addr;
    size_t section_table_index;

    LoadedSection(int32_t loaded_ram_addr_, size_t section_table_index_) {
        loaded_ram_addr = loaded_ram_addr_;
        section_table_index = section_table_index_;
    }

    bool operator<(const LoadedSection& rhs) {
        return loaded_ram_addr < rhs.loaded_ram_addr;
    }
};

static std::unordered_map<uint32_t, uint16_t> code_sections_by_rom{};
static std::unordered_map<uint32_t, uint16_t> patch_code_sections_by_rom{};
static std::vector<LoadedSection> loaded_sections{};
static std::unordered_map<int32_t, recomp_func_t*> func_map{};
static std::unordered_map<std::string, recomp_func_t*> base_exports{};
static std::unordered_map<std::string, recomp_func_ext_t*> ext_base_exports{};
static std::unordered_map<std::string, size_t> base_events;
static std::unordered_map<uint32_t, recomp_func_t*> manual_patch_symbols_by_vram;

extern "C" {
int32_t* section_addresses = nullptr;
}

// See recomp::overlays::set_overlay_relocation_enabled.
static bool overlay_relocation_enabled = true;

void recomp::overlays::set_overlay_relocation_enabled(bool enabled) {
    overlay_relocation_enabled = enabled;
}

void recomp::overlays::register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays) {
    sections_info = sections;
    overlays_info = overlays;
}

void recomp::overlays::register_patches(const char* patch, std::size_t size, SectionTableEntry* sections, size_t num_sections) {
    patch_code_sections = sections;
    num_patch_code_sections = num_sections;

    patch_data.resize(size);
    std::memcpy(patch_data.data(), patch, size);

    patch_code_sections_by_rom.reserve(num_patch_code_sections);
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        patch_code_sections_by_rom.emplace(patch_code_sections[i].rom_addr, i);
    }
}

void recomp::overlays::register_base_export(const std::string& name, recomp_func_t* func) {
    base_exports.emplace(name, func);
}

void recomp::overlays::register_ext_base_export(const std::string& name, recomp_func_ext_t* func) {
    ext_base_exports.emplace(name, func);
}

void recomp::overlays::register_base_exports(const FunctionExport* export_list) {
    std::unordered_map<uint32_t, recomp_func_t*> patch_func_vram_map{};

    // Iterate over all patch functions to set up a mapping of their vram address.
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const SectionTableEntry* cur_section = &patch_code_sections[patch_section_index];

        for (size_t func_index = 0; func_index < cur_section->num_funcs; func_index++) {
            const FuncEntry* cur_func = &cur_section->funcs[func_index];
            patch_func_vram_map.emplace(cur_section->ram_addr + cur_func->offset, cur_func->func);
        }
    }

    // Iterate over exports, using the vram mapping to create a name mapping.
    for (const FunctionExport* cur_export = &export_list[0]; cur_export->name != nullptr; cur_export++) {
        auto it = patch_func_vram_map.find(cur_export->ram_addr);
        if (it == patch_func_vram_map.end()) {
            assert(false && "Failed to find exported function in patch function sections!");
        }
        base_exports.emplace(cur_export->name, it->second);
    }
}

recomp_func_t* recomp::overlays::get_base_export(const std::string& export_name) {
    auto it = base_exports.find(export_name);
    if (it == base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

recomp_func_ext_t* recomp::overlays::get_ext_base_export(const std::string& export_name) {
    auto it = ext_base_exports.find(export_name);
    if (it == ext_base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

void recomp::overlays::register_base_events(char const* const* event_names) {
    for (size_t event_index = 0; event_names[event_index] != nullptr; event_index++) {
        base_events.emplace(event_names[event_index], event_index);
    }
}

size_t recomp::overlays::get_base_event_index(const std::string& event_name) {
    auto it = base_events.find(event_name);
    if (it == base_events.end()) {
        return (size_t)-1;
    }
    return it->second;
}

size_t recomp::overlays::num_base_events() {
    return base_events.size();
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_vrom_to_section_map() {
    return code_sections_by_rom;
}

uint32_t recomp::overlays::get_section_ram_addr(uint16_t code_section_index) {
    return sections_info.code_sections[code_section_index].ram_addr;
}

std::span<const RelocEntry> recomp::overlays::get_section_relocs(uint16_t code_section_index) {
    if (code_section_index < sections_info.num_code_sections) {
        const auto& section = sections_info.code_sections[code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

void recomp::overlays::add_loaded_function(int32_t ram, recomp_func_t* func) {
    func_map[ram] = func;
}

recomp_func_t* recomp::overlays::find_loaded_function(int32_t ram) {
    auto it = func_map.find(ram);
    return it == func_map.end() ? nullptr : it->second;
}

// Removes one loaded section from the function map and the loaded-section list,
// and returns an iterator to the entry after it.
static std::vector<LoadedSection>::iterator drop_loaded_section(std::vector<LoadedSection>::iterator it) {
    const SectionTableEntry& section = sections_info.code_sections[it->section_table_index];

    // Determine where each function was loaded to and remove that entry from the function map
    for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
        const auto& func = section.funcs[func_index];
        uint32_t func_address = func.offset + it->loaded_ram_addr;
        auto func_it = func_map.find(func_address);
        // Only remove the mapping if it still refers to this section. Two sections
        // loaded to overlapping addresses can define a function at the same address,
        // in which case whichever loaded later owns the entry and must keep it.
        if (func_it != func_map.end() && func_it->second == func.func) {
            func_map.erase(func_it);
        }
    }
    // Reset the section's address in the address table
    section_addresses[section.index] = section.ram_addr;
    // Remove the section from the loaded section map
    return loaded_sections.erase(it);
}

// See recomp::overlays::set_partial_eviction_enabled.
static bool partial_eviction_enabled = true;
static uint32_t partial_eviction_funcs = 0;

void recomp::overlays::set_partial_eviction_enabled(bool enabled) {
    partial_eviction_enabled = enabled;
}

uint32_t recomp::overlays::take_partial_eviction_func_count() {
    uint32_t count = partial_eviction_funcs;
    partial_eviction_funcs = 0;
    return count;
}

// See recomp::overlays::set_tail_clip_tolerated.
static bool tail_clip_tolerated = true;
static uint32_t tail_clip_funcs = 0;

void recomp::overlays::set_tail_clip_tolerated(bool tolerated) {
    tail_clip_tolerated = tolerated;
}

uint32_t recomp::overlays::take_tail_clip_func_count() {
    uint32_t count = tail_clip_funcs;
    tail_clip_funcs = 0;
    return count;
}

static bool trace_overlays() {
    static const bool enabled = getenv("HH_TRACE_OVERLAYS") != nullptr;
    return enabled;
}

// Removes from the function map only those functions of one loaded section that
// [start, end) actually overwrites, leaving the section loaded and the rest of
// its functions callable.
//
// This is what the hardware does. A load into the middle of a larger overlay's
// region destroys the bytes it covers and nothing else; the code above and below
// it is still sitting in rdram, still valid, and the game is free to call it.
// Dropping the whole section instead turns every one of those survivors into a
// hard `get_function` exit.
static void drop_overwritten_functions(const LoadedSection& loaded, uint32_t start, uint32_t end) {
    const SectionTableEntry& section = sections_info.code_sections[loaded.section_table_index];

    for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
        const auto& func = section.funcs[func_index];
        uint32_t func_start = func.offset + loaded.loaded_ram_addr;
        uint32_t func_end = func_start + func.rom_size;

        // A function is destroyed if a real instruction of it is overwritten.
        if (func_start >= end || func_end <= start) {
            continue;
        }

        // ...but a load whose only casualty is the function's FINAL WORD has not
        // destroyed a real instruction. That word is the delay slot of the
        // returning `jr $ra`; every instruction that does the function's work is
        // intact, and on hardware the function still runs and still returns.
        //
        // This is not a tolerance for damage, because the guest's copy of that
        // word never executes here. The recompiled function is host code
        // translated from all of the original words at build time, so what the
        // clip leaves in rdram changes nothing about what running it does. And
        // the clip is not the game overwriting code it means to replace: it is a
        // neighbouring overlay's base address landing on the tail padding of the
        // slot's previous tenant. `.file_55` loads at 0x803757E0, four bytes
        // inside `func_803757B0_8193D0` (0x803757B0-0x803757E4) -- 12 real
        // instructions untouched.
        //
        // Dropping it anyway is what a strict guard does, and it costs the whole
        // function: the game calls 0x803757B0 immediately afterwards and the port
        // exits in `get_function`. Registering `.file_56` is what made this reach
        // daylight -- before that, the entry this rule drops was never added.
        //
        // `start` alone decides it: an overlap that begins at or after the last
        // word cannot touch anything before it, whatever `end` is. Single-word
        // functions are excluded -- their last word is also their first.
        if (tail_clip_tolerated && func.rom_size > 4 && start >= func_end - 4) {
            tail_clip_funcs++;
            if (trace_overlays()) {
                fprintf(stderr, "[overlay] tail clip: %08X-%08X keeps its entry, load %08X-%08X"
                                " takes only its last word\n",
                        func_start, func_end, start, end);
                fflush(stderr);
            }
            continue;
        }

        auto func_it = func_map.find(func_start);
        // As in drop_loaded_section: only drop the mapping if it still refers to
        // this section, since a later load may already own that address.
        if (func_it != func_map.end() && func_it->second == func.func) {
            func_map.erase(func_it);
            partial_eviction_funcs++;
        }
    }
}

void load_overlay(size_t section_table_index, int32_t ram) {
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
    }

    loaded_sections.emplace_back(ram, section_table_index);
    if (overlay_relocation_enabled) {
        section_addresses[section.index] = ram;
    }
}

static void load_special_overlay(const SectionTableEntry& section, int32_t ram) {
    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
    }
}

static void load_patch_functions() {
    if (patch_code_sections == nullptr) {
        debug_printf("[Patch] No patch section was registered\n");
        return;
    }
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        load_special_overlay(patch_code_sections[i], patch_code_sections[i].ram_addr);
    }
}

void recomp::overlays::read_patch_data(uint8_t* rdram, gpr patch_data_address) {
    for (size_t i = 0; i < patch_data.size(); i++) {
        MEM_B(i, patch_data_address) = patch_data[i];
    }
}

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size) {
    // Search for the first section that's included in the loaded rom range
    // Sections were sorted by `init_overlays` so we can use the bounds functions
    auto lower = std::lower_bound(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections], rom,
        [](const SectionTableEntry& entry, uint32_t addr) {
            return entry.rom_addr < addr;
        }
    );
    auto upper = std::upper_bound(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections], (uint32_t)(rom + size),
        [](uint32_t addr, const SectionTableEntry& entry) {
            return addr < entry.size + entry.rom_addr;
        }
    );
    // Load the overlays that were found
    for (auto it = lower; it != upper; ++it) {
        load_overlay(std::distance(&sections_info.code_sections[0], it), it->rom_addr - rom + ram_addr);
    }
}

extern "C" void unload_overlay_by_id(uint32_t id) {
    uint32_t section_table_index = overlays_info.table[id];
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    auto find_it = std::find_if(loaded_sections.begin(), loaded_sections.end(), [section_table_index](const LoadedSection& s) { return s.section_table_index == section_table_index; });

    if (find_it != loaded_sections.end()) {
        // Determine where each function was loaded to and remove that entry from the function map
        for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
            const auto& func = section.funcs[func_index];
            uint32_t func_address = func.offset + find_it->loaded_ram_addr;
            func_map.erase(func_address);
        }
        // Reset the section's address in the address table
        section_addresses[section.index] = section.ram_addr;
        // Remove the section from the loaded section map
        loaded_sections.erase(find_it);
    }
}

extern "C" void load_overlay_by_id(uint32_t id, uint32_t ram_addr) {
    uint32_t section_table_index = overlays_info.table[id];
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];
    int32_t prev_address = section_addresses[section.index];
    if (/*ram_addr >= 0x80000000 && ram_addr < 0x81000000) {*/ prev_address == section.ram_addr) {
        load_overlay(section_table_index, ram_addr);
    }
    else {
        int32_t new_address = prev_address + ram_addr;
        unload_overlay_by_id(id);
        load_overlay(section_table_index, new_address);
    }
}

extern "C" void unload_overlays(int32_t ram_addr, uint32_t size) {
    for (auto it = loaded_sections.begin(); it != loaded_sections.end();) {
        const auto& section = sections_info.code_sections[it->section_table_index];

        // Check if the unloaded region overlaps with the loaded section
        if (ram_addr < (it->loaded_ram_addr + section.size) && (ram_addr + size) >= it->loaded_ram_addr) {
            // Check if the section isn't entirely in the loaded region
            if (ram_addr > it->loaded_ram_addr || (ram_addr + size) < (it->loaded_ram_addr + section.size)) {
                fprintf(stderr,
                    "Cannot partially unload section\n"
                    "  rom: 0x%08X size: 0x%08X loaded_addr: 0x%08X\n"
                    "  unloaded_ram: 0x%08X unloaded_size : 0x%08X\n",
                        section.rom_addr, section.size, it->loaded_ram_addr, ram_addr, size);
                assert(false);
                std::exit(EXIT_FAILURE);
            }
            // Determine where each function was loaded to and remove that entry from the function map
            for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
                const auto& func = section.funcs[func_index];
                uint32_t func_address = func.offset + it->loaded_ram_addr;
                func_map.erase(func_address);
            }
            // Reset the section's address in the address table
            section_addresses[section.index] = section.ram_addr;
            // Remove the section from the loaded section map
            it = loaded_sections.erase(it);
            // Skip incrementing the iterator
            continue;
        }
        ++it;
    }
}

// Drops whatever the range [ram_addr, ram_addr + size) destroys, section by
// section.
//
// `unload_overlays` cannot express this at all: it treats a partially covered
// section as a programming error and exits, and these overlay slots overlap
// partially by construction.
//
// A section the range covers *entirely* goes as a unit. A section it covers
// only in part keeps its identity and loses only the functions whose bytes were
// actually overwritten -- because that is what the load did to rdram. `.file_54`
// loads 0xA5F0 bytes at 0x803837E0, inside the 0x343A0 that `.file_56` occupies
// from 0x80358820; that overwrites 22 of `.file_56`'s 454 functions and leaves
// 432 of them untouched and still perfectly callable, `func_803757B0_8193D0`
// among them. Dropping the section whole made every one of those 432 a
// `Failed to find function` abort.
//
// Returns how many sections were dropped in full; the functions dropped out of
// surviving sections are counted separately, via
// recomp::overlays::take_partial_eviction_func_count.
extern "C" uint32_t unload_overlapping_overlays(int32_t ram_addr, uint32_t size) {
    uint32_t unload_start = (uint32_t)ram_addr;
    uint32_t unload_end = unload_start + size;
    uint32_t num_unloaded = 0;

    for (auto it = loaded_sections.begin(); it != loaded_sections.end();) {
        const auto& section = sections_info.code_sections[it->section_table_index];
        uint32_t section_start = (uint32_t)it->loaded_ram_addr;
        uint32_t section_end = section_start + section.size;

        if (unload_start < section_end && section_start < unload_end) {
            bool covers_whole_section = unload_start <= section_start && unload_end >= section_end;
            if (partial_eviction_enabled && !covers_whole_section) {
                drop_overwritten_functions(*it, unload_start, unload_end);
                ++it;
                continue;
            }
            it = drop_loaded_section(it);
            num_unloaded++;
            // Skip incrementing the iterator
            continue;
        }
        ++it;
    }

    return num_unloaded;
}

// Registers an overlay that arrived by a cart DMA nothing announced.
//
// Hybrid Heaven has TWO loaders for its Nisitenma-Ichigo archive, and the port
// patches one. Both take (file_id, dest), both bound the id at 0x271, and both
// read the same two tables -- the file table at 0x80038FE0 for the ROM address
// and the load-address table at 0x80037C5C for the extent:
//
//   func_8000469C_529C   the blocking loader. Patched in patches/required.c,
//                        which announces the load before moving the bytes.
//   func_80004838_5438   an asynchronous streaming variant, which records the
//                        transfer at D_800892B0+0x42AD.. and returns. NOT
//                        patched, and nothing announces what it loads.
//
// Those two are the whole class: a scan of every function in the game for a
// reference to the file table finds exactly these, and no other translation
// unit references it at all. So a load through the second one leaves the
// recompiler with no mapping from the overlay's vram to its native code, and
// the first indirect call into it aborts in get_function.
//
// That is not hypothetical -- it is the `Failed to find function at 0x803757B0`
// abort. 0x803757B0 is func_803757B0_8193D0 in .file_56 (archive entry 56,
// loader file_id 57, ROM 0x7FC440, slot 0x80358820). The game reaches it through
// a function pointer that lives at vram 0x803883EC, and the word 0x803757B0
// occurs exactly ONCE in the entire ROM: at ROM 0x82C00C, inside .file_56's own
// data, which is what maps to 0x803883EC when .file_56 is resident. Nothing
// computes the address arithmetically either. So the game could only have read
// that pointer out of .file_56's data, .file_56's bytes were therefore in rdram
// -- and no [overlay] line in either traced crash ever registered ROM 0x7FC440.
// The bytes arrived; the announcement did not.
//
// do_dma is the one place that is exhaustive by construction: every PI DMA
// passes through it whatever the guest-side path, so this needs no dataflow
// closure over callers and no second loader reimplemented in MIPS.
//
// The load is CHUNKED, which is the whole difficulty. func_80004838_5438 keeps
// its transfer state in D_800892B0: 0x42B0 the current ROM address, 0x42B4 the
// current destination, 0x42B8 what is left, 0x42CC the chunk size. Each call
// DMAs one chunk, advances all three by the chunk, and returns; only when the
// remainder fits in a single chunk does it transfer the rest. A 0x343A0 overlay
// therefore arrives as a run of smaller DMAs, of which only the first begins at
// the section's rom_addr and none but the last completes it. So this cannot test
// a single DMA and decide -- it has to follow the run.
//
// The cost of being exhaustive is that most cart DMAs are not overlay loads at
// all, and claiming one that isn't would be worse than the bug. Two conditions
// gate every chunk, and both are cheap and read nothing mutable:
//
//   1. the chunk's ROM range lies inside a known code section;
//   2. the chunk lands where that section's bytes would have to go for the load
//      to be placing it at its own link address -- ram - (rom - rom_addr) ==
//      ram_addr.
//
// (2) is exact rather than a heuristic: tools/verify_overlay_hook.py asserts for
// all 92 overlays that the load-address table's `start` IS the section's
// ram_addr, and that each file's ROM range covers its own section and no other.
// The pair excludes every fixed destination in the game's own loader audit -- the
// two 0x2000 ROM-streamer buffers at 0x80089518/0x8008B518, the 0x801077E0 table
// read, and the six audio banks from 0x80191520. The streamer is the one worth
// naming, because it does read ROM that lies inside a section: its 0x210 read at
// rom 0x5D280 is inside .main, but it lands at 0x80089518, which implies a base
// of 0x8002D2F8 against .main's 0x80000460, so (2) refuses it.
//
// Only a chunk that passes both advances the tracker below, and the section is
// registered at the moment its bytes are all present -- not before, so nothing
// half-fetched is ever callable, and not later, so it is callable by the time the
// game's own load returns.
//
// KNOWN LIMIT, and the reason `rejected_out` exists: (2) does not hold for a
// RELOCATED load. The game does relocate overlays -- .file_23 has been observed
// loaded both at its slot 0x801BF1A0 and at 0x801FA948 in the same run -- and one
// arriving that way through the unannounced path would be refused here rather
// than registered. Nothing yet shows that happening; if it ever does, it shows
// up as a rejection line in the DMA trace rather than as silence.
//
// This never double-registers. The patched loader announces its load *before*
// the DMA, so by the time do_dma sees the same transfer the section is already
// in loaded_sections at that address and is skipped.
extern "C" uint32_t register_unannounced_overlays(uint32_t rom, int32_t ram_addr, uint32_t size,
                                                 UnannouncedOverlay* out) {
    if (out != nullptr) {
        *out = UnannouncedOverlay{};
    }
    if (size == 0 || sections_info.num_code_sections == 0) {
        return 0;
    }

    // Condition 1: the section whose ROM range contains this chunk.
    //
    // Deliberately NOT the lower_bound/upper_bound pair that load_overlays uses.
    // That idiom is only sound for a range that starts on a section boundary, and
    // this function is offered every cart DMA in the game. A read starting in the
    // MIDDLE of a section inverts the two bounds -- for the streamer's read at rom
    // 0x5D280, inside .main, lower_bound returns the first overlay while
    // upper_bound returns .main, so upper < lower and `for (it = lower; it !=
    // upper; ++it)` walks off the end of the section array. That was a 0xC0000005
    // on the third overlay load of a plain boot.
    //
    // This is the ordinary containing-interval search instead: the last section
    // starting at or before `rom`, then a bounds check. init_overlays sorted
    // code_sections by rom_addr and the ranges are disjoint, so it is exact.
    const SectionTableEntry* begin = &sections_info.code_sections[0];
    const SectionTableEntry* end = begin + sections_info.num_code_sections;
    auto it = std::upper_bound(begin, end, rom,
        [](uint32_t addr, const SectionTableEntry& entry) {
            return addr < entry.rom_addr;
        }
    );
    if (it == begin) {
        return 0;
    }
    --it;
    if (rom >= it->rom_addr + it->size) {
        return 0;  // in a gap between sections
    }

    const SectionTableEntry& section = *it;
    size_t section_table_index = (size_t)(it - begin);

    // Condition 2: the base this chunk implies is the section's link address.
    int32_t implied_base = ram_addr - (int32_t)(rom - section.rom_addr);
    if (implied_base != (int32_t)section.ram_addr) {
        if (out != nullptr) {
            out->declined_ram = (uint32_t)implied_base;
        }
        return 0;
    }

    // Past this point only a genuine overlay-load chunk gets through, so the
    // tracker and the registration can afford a lock. do_dma runs on whichever
    // guest thread started the transfer, and loaded_sections is also mutated by
    // the patched loader on the game thread.
    static std::mutex claim_mutex;
    std::lock_guard<std::mutex> lock(claim_mutex);

    // Contiguous-progress tracker for the run of chunks. The loader only ever
    // advances forward and contiguously, so a high-water mark is enough: a chunk
    // that does not begin exactly where the last one ended restarts the run.
    static size_t pending_section = (size_t)-1;
    static uint32_t pending_covered_end = 0;

    if (pending_section != section_table_index || rom != pending_covered_end) {
        // A run only ever starts at the section's own beginning.
        if (rom != section.rom_addr) {
            pending_section = (size_t)-1;
            return 0;
        }
        pending_section = section_table_index;
        pending_covered_end = section.rom_addr;
    }

    pending_covered_end = rom + size;

    uint32_t section_end = section.rom_addr + section.size;
    if (pending_covered_end < section_end) {
        return 0;  // still arriving
    }

    pending_section = (size_t)-1;

    // Already announced -- the patched loader's own whole-file DMA lands here,
    // having registered the section just before moving the bytes.
    bool already_loaded = std::any_of(loaded_sections.begin(), loaded_sections.end(),
        [section_table_index, &section](const LoadedSection& s) {
            return s.section_table_index == section_table_index
                && s.loaded_ram_addr == (int32_t)section.ram_addr;
        }
    );
    if (already_loaded) {
        return 0;
    }

    // Logged before the mutation rather than after it, so a fault inside the
    // eviction or the load still names the DMA that caused it.
    if (getenv("HH_TRACE_DMA") != nullptr) {
        fprintf(stderr, "[overlay] claiming unannounced: rom %08X -> ram %08X size %08X"
                        " (section %zu, last chunk rom %08X size %08X)\n",
                section.rom_addr, section.ram_addr, section.size,
                section_table_index, rom, size);
        fflush(stderr);
    }

    // Drop whatever these bytes destroyed, exactly as the patched loader's
    // eviction does, and for the same reason: the game never announces that a
    // slot's previous tenant is gone. The range is the section's own extent, since
    // that is what the completed run wrote. The section being registered below is
    // not loaded, so this cannot drop it.
    unload_overlapping_overlays((int32_t)section.ram_addr, section.size);

    load_overlay(section_table_index, (int32_t)section.ram_addr);
    if (out != nullptr) {
        out->section_rom = section.rom_addr;
        out->section_ram = section.ram_addr;
        out->section_size = section.size;
    }

    return 1;
}

void recomp::overlays::init_overlays() {
    func_map.clear();
    section_addresses = (int32_t *)calloc(sections_info.total_num_sections, sizeof(int32_t));
    
    // Sort the executable sections by rom address
    std::sort(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections],
        [](const SectionTableEntry& a, const SectionTableEntry& b) {
            return a.rom_addr < b.rom_addr;
        }
    );

    for (size_t section_index = 0; section_index < sections_info.num_code_sections; section_index++) {
        SectionTableEntry* code_section = &sections_info.code_sections[section_index];

        section_addresses[sections_info.code_sections[section_index].index] = code_section->ram_addr;
        code_sections_by_rom[code_section->rom_addr] = section_index;        
    }

    load_patch_functions();
}

// Finds a function given a section's index and the function's offset into the section.
bool recomp::overlays::get_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (code_section_index >= sections_info.num_code_sections) {
        return false;
    }

    SectionTableEntry* section = &sections_info.code_sections[code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

void recomp::overlays::register_manual_patch_symbols(const ManualPatchSymbol* manual_patch_symbols) {
    for (size_t i = 0; manual_patch_symbols[i].func != nullptr; i++) {
        if (!manual_patch_symbols_by_vram.emplace(manual_patch_symbols[i].ram_addr, manual_patch_symbols[i].func).second) {
            printf("Duplicate manual patch symbol address: %08X\n", manual_patch_symbols[i].ram_addr);
            ultramodern::error_handling::message_box("Duplicate manual patch symbol address (syms.ld)!");
            assert(false && "Duplicate manual patch symbol address (syms.ld)!");
            ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
        }
    }
}

// TODO use N64Recomp::is_manual_patch_symbol instead after updating submodule.
bool is_manual_patch_symbol(uint32_t vram) {
    return vram >= 0x8F000000 && vram < 0x90000000;
}

// Finds a function given a section's index and the function's offset into the section and returns its native pointer.
recomp_func_t* recomp::overlays::get_func_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset) {
    FuncEntry entry;
    
    if (get_func_entry_by_section_index_function_offset(code_section_index, function_offset, entry)) {
        return entry.func;
    }

    if (code_section_index == N64Recomp::SectionAbsolute && is_manual_patch_symbol(function_offset)) {
        auto find_it = manual_patch_symbols_by_vram.find(function_offset);
        if (find_it != manual_patch_symbols_by_vram.end()) {
            return find_it->second;
        }
    }

    return nullptr;
}

// Finds a function given a section's rom address and the function's vram address.
recomp_func_t* recomp::overlays::get_func_by_section_rom_function_vram(uint32_t section_rom, uint32_t function_vram) {
    auto find_section_it = code_sections_by_rom.find(section_rom);
    if (find_section_it == code_sections_by_rom.end()) {
        return nullptr;
    }

    SectionTableEntry* section = &sections_info.code_sections[find_section_it->second];
    int32_t func_offset = function_vram - section->ram_addr;
    
    return get_func_by_section_index_function_offset(find_section_it->second, func_offset);
}

extern "C" recomp_func_t * get_function(int32_t addr) {
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        fprintf(stderr, "Failed to find function at 0x%08X\n", addr);
        assert(false);
        std::exit(EXIT_FAILURE);
    }
    return func_find->second;
}

std::unordered_map<recomp_func_t*, recomp::overlays::BasePatchedFunction> recomp::overlays::get_base_patched_funcs() {
    std::unordered_map<recomp_func_t*, BasePatchedFunction> ret{};

    // Collect the set of all functions in the patches.
    std::unordered_map<recomp_func_t*, BasePatchedFunction> all_patch_funcs{};
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const auto& patch_section = patch_code_sections[patch_section_index];
        for (size_t func_index = 0; func_index < patch_section.num_funcs; func_index++) {
            all_patch_funcs.emplace(patch_section.funcs[func_index].func, BasePatchedFunction{ .patch_section = patch_section_index, .function_index = func_index });
        }
    }

    // Check every vanilla function against the full patch function set.
    // Any functions in both are patched.
    for (size_t code_section_index = 0; code_section_index < sections_info.num_code_sections; code_section_index++) {
        const auto& code_section = sections_info.code_sections[code_section_index];
        for (size_t func_index = 0; func_index < code_section.num_funcs; func_index++) {
            recomp_func_t* cur_func = code_section.funcs[func_index].func;
            // If this function also exists in the patches function set then it's a vanilla function that was patched.
            auto find_it = all_patch_funcs.find(cur_func);
            if (find_it != all_patch_funcs.end()) {
                ret.emplace(cur_func, find_it->second);
            }
        }
    }

    return ret;
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_patch_vrom_to_section_map() {
    return patch_code_sections_by_rom;
}

uint32_t recomp::overlays::get_patch_section_ram_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].ram_addr;
    }
    assert(false);
    return -1;
}

uint32_t recomp::overlays::get_patch_section_rom_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].rom_addr;
    }
    assert(false);
    return -1;
}

const FuncEntry* recomp::overlays::get_patch_function_entry(uint16_t patch_code_section_index, size_t function_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        if (function_index < section.num_funcs) {
            return &section.funcs[function_index];
        }
    }
    assert(false);
    return nullptr;
}

// Finds a base patched function given a patch section's index and the function's offset into the section.
bool recomp::overlays::get_patch_func_entry_by_section_index_function_offset(uint16_t patch_code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (patch_code_section_index >= num_patch_code_sections) {
        return false;
    }

    SectionTableEntry* section = &patch_code_sections[patch_code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

std::span<const RelocEntry> recomp::overlays::get_patch_section_relocs(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

std::span<const uint8_t> recomp::overlays::get_patch_binary() {
    return std::span{ reinterpret_cast<const uint8_t*>(patch_data.data()), patch_data.size() };
}
