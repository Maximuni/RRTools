/* menu_bridge.h - Shared settings and WebView2 menu bridge. */
#ifndef MENU_BRIDGE_H
#define MENU_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char rgb[3];
    unsigned char pad;
} BR_Color;

#define BR_COLOR_COUNT 18

#define BR_SETTINGS(X) \
    X(enabled,               1,   0,    1) \
    X(show_box,              1,   0,    1) \
    X(box_style,             0,   0,    1) \
    X(show_skeleton,         1,   0,    1) \
    X(show_name,             1,   0,    1) \
    X(show_armor,            1,   0,    1) \
    X(show_bars,             1,   0,    1) \
    X(show_distance,         1,   0,    1) \
    X(show_weapon,           1,   0,    1) \
    X(show_head,             1,   0,    1) \
    X(show_self,             0,   0,    1) \
    X(show_clan,             1,   0,    1) \
    X(show_police,           1,   0,    1) \
    X(party_filter,          1,   0,    1) \
    X(cham_on,               0,   0,    1) \
    X(vischeck_colors,       1,   0,    1) \
    X(snaplines_on,          0,   0,    1) \
    X(show_hiding_spots,     0,   0,    1) \
    X(stream_proof,          0,   0,    1) \
    X(max_distance,      50000, 500, 100000) \
    X(skel_distance,     15000, 500, 100000) \
    X(line_w,                2,   1,    4) \
    X(radar_on,              1,   0,    1) \
    X(radar_rotate,          0,   0,    1) \
    X(radar_range,         150,  30,  600) \
    X(map_key,            0x4D,   0,  254) \
    X(aa_on,                 0,   0,    1) \
    X(aa_show_fov,           1,   0,    1) \
    X(aa_fov_px,           140,  10, 1200) \
    X(aa_key,             0x02,   1,  254) \
    X(aa_smooth,             6,   1,   30) \
    X(aa_aim_cal,          100,  10, 1000) \
    X(aa_dist,           25000, 500, 100000) \
    X(aa_target_clan,        0,   0,    1) \
    X(aa_target_police,      0,   0,    1) \
    X(aa_bone,               0,   0,    9) \
    X(aa_los_on,             1,   0,    1) \
    X(aa_closest_first,      0,   0,    1) \
    X(tb_on,                 0,   0,    1) \
    X(tb_key,                0,   0,  254) \
    X(tb_delay_ms,          35,   0,  300) \
    X(tb_fov_px,             8,   2,   30) \
    X(show_loot,             0,   0,    1) \
    X(show_loot_nodes,       0,   0,    1) \
    X(loot_tier_none,        1,   0,    1) \
    X(loot_tier_common,      1,   0,    1) \
    X(loot_tier_uncommon,    1,   0,    1) \
    X(loot_tier_rare,        1,   0,    1) \
    X(loot_tier_epic,        1,   0,    1) \
    X(loot_tier_legendary,   1,   0,    1) \
    X(loot_tier_exotic,      1,   0,    1) \
    X(vehicle_chams,         0,   0,    1) \
    X(show_containers,       0,   0,    1) \
    X(show_doors,            0,   0,    1) \
    X(show_dropped_loot,     0,   0,    1) \
    X(show_vehicle_loot,     0,   0,    1) \
    X(radar_world,           1,   0,    1) \
    X(show_crouch,           0,   0,    1) \
    X(show_health,           0,   0,    1) \
    X(show_vehicle_markers,  1,   0,    1) \
    X(show_vehicle_occupants,1,   0,    1) \
    X(instant_loot,          1,   0,    1)

typedef struct BR_Config {
#define BR_STRUCT_FIELD(field, initial, minimum, maximum) int field;
    BR_SETTINGS(BR_STRUCT_FIELD)
#undef BR_STRUCT_FIELD
    int menu_open;

    BR_Color color[BR_COLOR_COUNT];

    int telem_targets;
    int telem_cham_n;
    int telem_los_state;
    int telem_game_pid;
} BR_Config;

static inline int br_loot_rarity_visible(const BR_Config* cfg, int rarity) {
    switch (rarity) {
        case 0: return cfg->loot_tier_none;
        case 1: return cfg->loot_tier_common;
        case 2: return cfg->loot_tier_uncommon;
        case 3: return cfg->loot_tier_rare;
        case 4: return cfg->loot_tier_epic;
        case 5: return cfg->loot_tier_legendary;
        case 6: return cfg->loot_tier_exotic;
        default: return 1;
    }
}

BR_Config* br_get(void);
void       br_start_menu(void);
void       br_stop_menu(void);
void       br_set_menu_visible(int show);
void       br_request_toggle(void);
int        br_config_set(BR_Config* b, const char* key, int value);
int        br_config_save(void);
int        br_config_load(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_BRIDGE_H */