#include "recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "gauntlet_game.h"

void gauntlet::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}

// None of Gauntlet's code sections share an address range, so every section's functions stay registered
// for the whole run. The game inflates its own code, so no ROM DMA would ever trigger the normal loading path.
void gauntlet::load_all_sections() {
    for (size_t i = 0; i < ARRLEN(section_table); i++) {
        load_overlays(section_table[i].rom_addr, (int32_t)section_table[i].ram_addr, section_table[i].size);
    }
}
