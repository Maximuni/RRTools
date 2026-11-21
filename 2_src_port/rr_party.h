#ifndef RR_PARTY_H
#define RR_PARTY_H

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define RR_MAX_PARTY_NAMES 32
#define RR_MAX_PARTY_POS   16

typedef struct {
    int   active;
    float x, y, z;
    DWORD last_seen_tick;
} RR_PARTY_TRACKED_POS;

static CRITICAL_SECTION     g_party_cs;
static volatile LONG        g_party_cs_init = 0;
static char                 g_party_names[RR_MAX_PARTY_NAMES][64];
static int                  g_party_name_count = 0;
static DWORD                g_party_last_load = 0;
static RR_PARTY_TRACKED_POS g_party_pos[RR_MAX_PARTY_POS];

static float g_cand_best_dist = 999999.0f;
static float g_cand_wx = 0.0f, g_cand_wy = 0.0f, g_cand_wz = 0.0f;
static char  g_cand_name[64] = {0};
static int   g_cand_valid = 0;

static void rr_party_ensure_init(void) {
    if (InterlockedCompareExchange(&g_party_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_party_cs);
        InterlockedExchange(&g_party_cs_init, 2);
    } else {
        while (g_party_cs_init < 2) SwitchToThread();
    }
}

static void rr_party_reload_file_locked(void) {
    DWORD now = GetTickCount();
    if (g_party_last_load != 0 && (now - g_party_last_load) < 3000)
        return;
    g_party_last_load = now;

    FILE *f = fopen("party.txt", "r");
    if (!f) {
        f = fopen("party.txt", "a");
        if (f) fclose(f);
        return;
    }

    g_party_name_count = 0;
    char line[128];
    while (fgets(line, sizeof(line), f) && g_party_name_count < RR_MAX_PARTY_NAMES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
        if (len > 0 && line[0] != '#') {
            strncpy(g_party_names[g_party_name_count], line, 63);
            g_party_names[g_party_name_count][63] = '\0';
            g_party_name_count++;
        }
    }
    fclose(f);
}

static void rr_party_save_file_locked(void) {
    FILE *f = fopen("party.txt", "w");
    if (!f) return;
    for (int i = 0; i < g_party_name_count; i++) {
        if (g_party_names[i][0])
            fprintf(f, "%s\n", g_party_names[i]);
    }
    fclose(f);
}

static void rr_party_frame_tick(void) {
    rr_party_ensure_init();
    EnterCriticalSection(&g_party_cs);
    rr_party_reload_file_locked();

    if ((GetAsyncKeyState(VK_F2) & 1) && g_cand_valid) {
        int removed = 0;
        if (g_cand_name[0] != '\0') {
            for (int i = 0; i < g_party_name_count; i++) {
                if (_stricmp(g_party_names[i], g_cand_name) == 0) {
                    for (int j = i; j < g_party_name_count - 1; j++)
                        memcpy(g_party_names[j], g_party_names[j + 1], 64);
                    g_party_name_count--;
                    removed = 1;
                    break;
                }
            }
            if (!removed && g_party_name_count < RR_MAX_PARTY_NAMES) {
                strncpy(g_party_names[g_party_name_count], g_cand_name, 63);
                g_party_names[g_party_name_count][63] = '\0';
                g_party_name_count++;
            }
            rr_party_save_file_locked();
        }

        int pos_removed = 0;
        for (int i = 0; i < RR_MAX_PARTY_POS; i++) {
            if (!g_party_pos[i].active) continue;
            float dx = g_party_pos[i].x - g_cand_wx;
            float dy = g_party_pos[i].y - g_cand_wy;
            float dz = g_party_pos[i].z - g_cand_wz;
            if ((dx * dx + dy * dy + dz * dz) < 9.0f) {
                g_party_pos[i].active = 0;
                pos_removed = 1;
            }
        }
        if (!removed && !pos_removed) {
            for (int i = 0; i < RR_MAX_PARTY_POS; i++) {
                if (!g_party_pos[i].active) {
                    g_party_pos[i].active = 1;
                    g_party_pos[i].x = g_cand_wx;
                    g_party_pos[i].y = g_cand_wy;
                    g_party_pos[i].z = g_cand_wz;
                    g_party_pos[i].last_seen_tick = GetTickCount();
                    break;
                }
            }
        }
        Beep(removed || pos_removed ? 400 : 900, 120);
    }

    g_cand_best_dist = 999999.0f;
    g_cand_valid = 0;
    LeaveCriticalSection(&g_party_cs);
}

static void rr_party_register_screen_candidate(float sx, float sy, float cx, float cy,
                                               float wx, float wy, float wz, const char *name)
{
    float dx = sx - cx;
    float dy = sy - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 250.0f && dist < g_cand_best_dist) {
        g_cand_best_dist = dist;
        g_cand_wx = wx;
        g_cand_wy = wy;
        g_cand_wz = wz;
        if (name && name[0]) {
            strncpy(g_cand_name, name, 63);
            g_cand_name[63] = '\0';
        } else {
            g_cand_name[0] = '\0';
        }
        g_cand_valid = 1;
    }
}

static int rr_party_is_member(const char *name, float wx, float wy, float wz) {
    rr_party_ensure_init();
    int is_ally = 0;
    DWORD now = GetTickCount();

    EnterCriticalSection(&g_party_cs);
    rr_party_reload_file_locked();

    if (name && name[0]) {
        for (int i = 0; i < g_party_name_count; i++) {
            if (g_party_names[i][0] &&
                (_stricmp(name, g_party_names[i]) == 0 || strstr(name, g_party_names[i]) != NULL)) {
                is_ally = 1;
                break;
            }
        }
    }

    for (int i = 0; i < RR_MAX_PARTY_POS; i++) {
        if (!g_party_pos[i].active) continue;
        if ((now - g_party_pos[i].last_seen_tick) > 60000) {
            g_party_pos[i].active = 0;
            continue;
        }
        float dx = g_party_pos[i].x - wx;
        float dy = g_party_pos[i].y - wy;
        float dz = g_party_pos[i].z - wz;
        if ((dx * dx + dy * dy + dz * dz) < 6.25f) {
            g_party_pos[i].x = wx;
            g_party_pos[i].y = wy;
            g_party_pos[i].z = wz;
            g_party_pos[i].last_seen_tick = now;
            is_ally = 1;
            break;
        }
    }

    LeaveCriticalSection(&g_party_cs);
    return is_ally;
}

#endif
