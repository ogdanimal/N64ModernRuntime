#ifndef __RECOMP_OVERLAYS_H__
#define __RECOMP_OVERLAYS_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <span>
#include "sections.h"

namespace recomp {
    namespace overlays {
        struct overlay_section_table_data_t {
            SectionTableEntry* code_sections;
            size_t num_code_sections;
            size_t total_num_sections;
        };

        struct overlays_by_index_t {
            int* table;
            size_t len;
        };

        void register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays);

        void register_patches(const char* patch_data, size_t patch_size, SectionTableEntry* code_sections, size_t num_sections);
        void register_base_export(const std::string& name, recomp_func_t* func);
        void register_ext_base_export(const std::string& name, recomp_func_ext_t* func);
        void register_base_exports(const FunctionExport* exports);
        void register_base_events(char const* const* event_names);
        void register_manual_patch_symbols(const ManualPatchSymbol* manual_patch_symbols);
        void read_patch_data(uint8_t* rdram, gpr patch_data_address);

        // Controls what `load_overlay` does with `section_addresses`, which the
        // recompiled RELOC_HI16/RELOC_LO16 read to resolve a relocatable section's
        // own absolute references.
        //
        // Enabled (the default), a section loaded away from its link address has
        // its references rebased onto wherever it was loaded -- correct for a game
        // that relocates an overlay after moving it, or maps it to a fixed virtual
        // address.
        //
        // Disabled, `section_addresses` keeps the link address whatever a load
        // passes. That is what a game which moves an overlay and does NOT relocate
        // it actually does on hardware: the copied instructions still carry their
        // original absolute addresses, so they reach the original copy's data no
        // matter where the copy runs from. Such a game can have the same section
        // live at two addresses at once, which a per-load address cannot describe
        // -- one of the two copies would always be rebased onto the other's data.
        void set_overlay_relocation_enabled(bool enabled);

        // Controls what `unload_overlapping_overlays` does with a loaded section
        // that a load overwrites only *part* of.
        //
        // Enabled (the default), only the functions whose bytes the load actually
        // destroys lose their entry in the function map; the section stays loaded
        // and the rest of it stays callable, which is what the same load does to
        // rdram on hardware.
        //
        // Disabled, any overlap drops the whole section. That is safe in the sense
        // that nothing stale can be reached, but it is wrong for a game that loads
        // a small overlay into the middle of a larger one's region: every function
        // of the larger overlay outside the overwritten range is still valid code
        // that the game may still call, and dropping it turns a legitimate call
        // into a `Failed to find function` exit.
        void set_partial_eviction_enabled(bool enabled);

        // Number of functions dropped out of surviving sections since the last
        // call, and resets the counter. Sections dropped whole are counted by
        // `unload_overlapping_overlays`'s return value instead.
        uint32_t take_partial_eviction_func_count();

        void init_overlays();
        const std::unordered_map<uint32_t, uint16_t>& get_vrom_to_section_map();
        uint32_t get_section_ram_addr(uint16_t code_section_index);
        std::span<const RelocEntry> get_section_relocs(uint16_t code_section_index);
        recomp_func_t* get_func_by_section_rom_function_vram(uint32_t section_rom, uint32_t function_vram);
        bool get_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out);
        recomp_func_t* get_func_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset);
        recomp_func_t* get_base_export(const std::string& export_name);
        recomp_func_ext_t* get_ext_base_export(const std::string& export_name);
        size_t get_base_event_index(const std::string& event_name);
        size_t num_base_events();

        void add_loaded_function(int32_t ram_addr, recomp_func_t* func);

        // Whether a vram address is currently callable. get_function answers the
        // same question but exits the process on a miss, so it cannot be used to
        // ASK -- only to call. Returns nullptr if nothing is mapped there.
        recomp_func_t* find_loaded_function(int32_t ram_addr);

        struct BasePatchedFunction {
            size_t patch_section;
            size_t function_index;
        };

        std::unordered_map<recomp_func_t*, BasePatchedFunction> get_base_patched_funcs();
        const std::unordered_map<uint32_t, uint16_t>& get_patch_vrom_to_section_map();
        uint32_t get_patch_section_ram_addr(uint16_t patch_code_section_index);
        uint32_t get_patch_section_rom_addr(uint16_t patch_code_section_index);
        const FuncEntry* get_patch_function_entry(uint16_t patch_code_section_index, size_t function_index);
        bool get_patch_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out);
        std::span<const RelocEntry> get_patch_section_relocs(uint16_t patch_code_section_index);
        std::span<const uint8_t> get_patch_binary();
    }
};

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);
extern "C" uint32_t unload_overlapping_overlays(int32_t ram_addr, uint32_t size);

// What register_unannounced_overlays claimed, or declined to claim.
//
// The three `section_*` fields describe the OVERLAY, not the DMA that completed
// it. Those are different numbers and confusing them misidentifies the overlay: a
// chunked load is claimed on its final chunk, so the triggering DMA is typically a
// short tail -- .file_56's was rom 0x82F440 size 0x13A0, against the section's rom
// 0x7FC440 size 0x343A0.
struct UnannouncedOverlay {
    // Valid when register_unannounced_overlays returned nonzero.
    uint32_t section_rom;
    uint32_t section_ram;
    uint32_t section_size;
    // Set instead when a chunk looked like an overlay load but implied a base
    // other than the section's link address, so a caller tracing DMA can see the
    // near-misses. Zero when nothing was declined.
    uint32_t declined_ram;
};

// Registers an overlay that a raw cart DMA delivered without anything having
// announced it. See the definition in overlays.cpp for the full rationale and
// for exactly which loads it will and will not claim.
//
// Returns the number of sections newly registered (0 or 1).
extern "C" uint32_t register_unannounced_overlays(uint32_t rom, int32_t ram_addr, uint32_t size,
                                                 UnannouncedOverlay* out);

#endif
