/* rr_aim.c - Target-region selection and bounded mouse-steering math. */
#include "rr_aim.h"
#include "menu_bridge.h"
#include "rr_cham_model.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "rr_party.h"
#include <math.h>
#include <string.h>

static int valid_joint(const RR_PLAYER* player, int bone) {
    return rr_rig_is_body_joint(bone) &&
           isfinite(player->bone[bone][0]) &&
           isfinite(player->bone[bone][1]) &&
           isfinite(player->bone[bone][2]);
}

static RR_V3 mesh_anchor(const RR_PLAYER* player, int bone) {
    const float* m = player->cham_skin[bone];
    const float* a = rr_cham_bones[bone].anchor;
    return (RR_V3){
        m[0] * a[0] + m[1] * a[1] + m[2] * a[2] + m[3],
        m[4] * a[0] + m[5] * a[1] + m[6] * a[2] + m[7],
        m[8] * a[0] + m[9] * a[1] + m[10] * a[2] + m[11]
    };
}

static int aim_axis(float error_px, float fov_deg, int viewport_h, int smooth,
                    int cal, unsigned elapsed_ms, int firing, float* carry) {
    if (!(fov_deg > 1.0f) || !(fov_deg < 179.0f) || viewport_h <= 0 ||
        !isfinite(error_px) || fabsf(error_px) < 0.05f) {
        *carry = 0.0f;
        return 0;
    }

    float scale = tanf(fov_deg * 3.14159265f / 360.0f) / 0.57735027f;
    float ref_error = error_px * (1080.0f / (float)viewport_h) * scale;
    float abs_err = fabsf(ref_error);

    if (abs_err < 0.40f) {
        *carry *= 0.4f;
        return 0;
    }

    if (smooth < 1) smooth = 1;
    if (smooth > 30) smooth = 30;
    if (cal < 10) cal = 10;
    if (cal > 1000) cal = 1000;
    if (elapsed_ms > 100) elapsed_ms = 100;

    float zone_scale = 1.0f;
    if (abs_err < 10.0f) {
        float t = abs_err / 10.0f;
        zone_scale = 0.42f + 0.58f * t;
    } else if (abs_err > 50.0f) {
        float extra = (abs_err - 50.0f) / 150.0f;
        if (extra > 0.30f) extra = 0.30f;
        zone_scale = 1.0f + extra;
    }

    float base_ms = (smooth <= 1) ? 1.0f : (11.0f + (float)(smooth - 1) * 8.5f);
    if (firing && smooth > 1) base_ms *= 1.12f;

    float response_ms = base_ms / zone_scale;
    float gain = (smooth <= 1) ? 1.0f : (1.0f - expf(-(float)elapsed_ms / response_ms));
    float counts = ref_error * ((float)cal / 100.0f) * gain + *carry;

    if ((counts > 0.0f && *carry < 0.0f) || (counts < 0.0f && *carry > 0.0f))
        counts = ref_error * ((float)cal / 100.0f) * gain;

    if (counts > 110.0f) counts = 110.0f;
    if (counts < -110.0f) counts = -110.0f;

    int step = (int)(counts >= 0.0f ? floorf(counts) : ceilf(counts));
    *carry = counts - (float)step;
    return step;
}

int rr_aim_bone_in_group(int group, int bone) {
    if (!rr_rig_is_body_joint(bone)) return 0;
    switch (group) {
        case RR_AIM_HEAD:    return bone >= RR_RIG_M_HEAD_1_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
        case RR_AIM_NECK:    return bone == RR_RIG_M_NECK_JNT;
        case RR_AIM_CHEST:   return bone == RR_RIG_M_SPINE_2_JNT || bone == RR_RIG_M_SPINE_3_JNT;
        case RR_AIM_PELVIS:  return bone == RR_RIG_M_SPINE_1_JNT;
        case RR_AIM_CLOSEST: return 1;
        case RR_AIM_ARMS:    return (bone >= RR_RIG_L_ARM_0_JNT && bone <= RR_RIG_L_ARMTWIST_JNT) ||
                                    (bone >= RR_RIG_R_ARM_0_JNT && bone <= RR_RIG_R_ARMTWIST_JNT);
        case RR_AIM_LEGS:    return (bone >= RR_RIG_L_LEG_1_JNT && bone <= RR_RIG_L_FOOT_2_JNT) ||
                                    (bone >= RR_RIG_R_LEG_1_JNT && bone <= RR_RIG_R_FOOT_2_JNT);
        case RR_AIM_FACE:    return bone >= RR_RIG_L_EYELIDUP_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
        case RR_AIM_HANDS:   return (bone >= RR_RIG_L_ARM_3_JNT && bone <= RR_RIG_L_THUMB_3_JNT) ||
                                    (bone >= RR_RIG_R_ARM_3_JNT && bone <= RR_RIG_R_THUMB_3_JNT);
        case RR_AIM_FEET:    return bone == RR_RIG_L_LEG_3_JNT || bone == RR_RIG_L_FOOT_1_JNT ||
                                    bone == RR_RIG_L_FOOT_2_JNT || bone == RR_RIG_R_LEG_3_JNT ||
                                    bone == RR_RIG_R_FOOT_1_JNT || bone == RR_RIG_R_FOOT_2_JNT;
        default:             return 0;
    }
}

static int specific_joint_point(const RR_PLAYER* player, int bone, RR_V3* out) {
    if (!player || !out || bone < 0 || bone >= RR_CHAM_BONES) return 0;
    if (!player->full_rig || player->nbones != RR_CHAM_BONES || !valid_joint(player, bone)) return 0;
    if (player->cham_valid) {
        *out = mesh_anchor(player, bone);
        return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
    }
    *out = (RR_V3){ player->bone[bone][0], player->bone[bone][1], player->bone[bone][2] };
    return 1;
}

static int limb_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                      int width, int height, RR_V3* out, int* out_bone, int (*clear)(RR_V3, RR_V3)) {
    static const int indices[6] = {
        RR_RIG_L_ARM_1_JNT, RR_RIG_L_ARM_2_JNT, RR_RIG_R_ARM_1_JNT, RR_RIG_R_ARM_2_JNT,
        RR_RIG_L_LEG_1_JNT, RR_RIG_L_LEG_2_JNT
    };
    int from = group == RR_AIM_ARMS ? 0 : (group == RR_AIM_LEGS ? 4 : 0);
    int to   = group == RR_AIM_ARMS ? 4 : (group == RR_AIM_LEGS ? 6 : 6);
    float cx = (float)width * 0.5f, cy = (float)height * 0.5f;
    float best_d = 1e30f;
    int found = 0, found_clear = 0;

    for (int i = from; i < to; ++i) {
        int bone = indices[i];
        if (!valid_joint(player, bone)) continue;
        RR_V3 point = player->cham_valid ? mesh_anchor(player, bone)
                                         : (RR_V3){ player->bone[bone][0], player->bone[bone][1], player->bone[bone][2] };
        float sx, sy;
        if (!game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float d = hypotf(sx - cx, sy - cy);
        int is_clear = !clear || clear(camera->pos, point);
        if ((is_clear && !found_clear) || (is_clear == found_clear && d < best_d)) {
            best_d = d;
            *out = point;
            if (out_bone) *out_bone = bone;
            found = 1;
            found_clear = is_clear;
        }
    }
    return found;
}

static int full_rig_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                          int width, int height, RR_V3* out, int* out_bone, int (*clear)(RR_V3, RR_V3)) {
    if (!player->full_rig || player->nbones != RR_CHAM_BONES || !camera) return 0;
    if (player->cham_valid && (group == RR_AIM_ARMS || group == RR_AIM_LEGS) &&
        limb_point(player, group, camera, width, height, out, out_bone, clear)) return 1;

    float cx = (float)width * 0.5f, cy = (float)height * 0.5f;
    float best_d = 1e30f;
    int found = 0, found_clear = 0;

    if (player->cham_valid) {
        for (int i = 0; i < (int)(sizeof(rr_cham_anchors) / sizeof(rr_cham_anchors[0])); ++i) {
            int bone = rr_cham_anchors[i].bone;
            if (!rr_aim_bone_in_group(group, bone) || !valid_joint(player, bone)) continue;
            RR_V3 point = mesh_anchor(player, bone);
            float sx, sy;
            if (!game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            float d = hypotf(sx - cx, sy - cy);
            int is_clear = !clear || clear(camera->pos, point);
            if ((is_clear && !found_clear) || (is_clear == found_clear && d < best_d)) {
                best_d = d;
                *out = point;
                if (out_bone) *out_bone = bone;
                found = 1;
                found_clear = is_clear;
            }
        }
        if (found) return 1;
    }

    for (int bone = 0; bone < RR_CHAM_BONES; ++bone) {
        if (!rr_aim_bone_in_group(group, bone) || !valid_joint(player, bone)) continue;
        RR_V3 point = { player->bone[bone][0], player->bone[bone][1], player->bone[bone][2] };
        float sx, sy;
        if (!game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float d = hypotf(sx - cx, sy - cy);
        int is_clear = !clear || clear(camera->pos, point);
        if ((is_clear && !found_clear) || (is_clear == found_clear && d < best_d)) {
            best_d = d;
            *out = point;
            if (out_bone) *out_bone = bone;
            found = 1;
            found_clear = is_clear;
        }
    }
    return found;
}

static int group_point_ex(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                          int width, int height, RR_V3* out, int* out_bone, int (*clear)(RR_V3, RR_V3)) {
    static const float standing[] = { 1.62f, 1.48f, 1.24f, 0.96f, 1.32f, 1.22f, 0.56f, 1.60f, 1.00f, 0.12f };
    static const float crouched[] = { 0.98f, 0.90f, 0.74f, 0.56f, 0.80f, 0.72f, 0.34f, 0.96f, 0.58f, 0.10f };
    if (!player || !out) return 0;
    if (out_bone) *out_bone = -1;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (full_rig_point(player, group, camera, width, height, out, out_bone, clear)) return 1;

    if (player->nbones > 0 && player->nbones <= RR_MAX_BONES) {
        if (group <= RR_AIM_PELVIS) {
            int j = player->bone_group[group];
            if (j >= 0 && j < player->nbones) {
                *out = (RR_V3){ player->bone[j][0], player->bone[j][1], player->bone[j][2] };
                return 1;
            }
        }
        if (group == RR_AIM_CLOSEST && camera) {
            float cx = (float)width * 0.5f, cy = (float)height * 0.5f;
            float best_d = 1e30f;
            int found = 0, found_clear = 0;
            for (int i = 0; i < 4; ++i) {
                int j = player->bone_group[i];
                if (j < 0 || j >= player->nbones) continue;
                RR_V3 pt = { player->bone[j][0], player->bone[j][1], player->bone[j][2] };
                float sx, sy;
                if (!game_w2s(camera, &pt, width, height, &sx, &sy)) continue;
                float d = hypotf(sx - cx, sy - cy);
                int is_clear = !clear || clear(camera->pos, pt);
                if ((is_clear && !found_clear) || (is_clear == found_clear && d < best_d)) {
                    best_d = d;
                    *out = pt;
                    found = 1;
                    found_clear = is_clear;
                }
            }
            if (found) return 1;
        }
    }

    if (group == RR_AIM_HEAD || group == RR_AIM_FACE) {
        *out = player->head;
        return 1;
    }
    *out = player->pos;
    out->y += (player->crouch == 1 ? crouched : standing)[group];
    return 1;
}

int rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                 int width, int height, RR_V3* out) {
    return group_point_ex(player, group, camera, width, height, out, NULL, NULL);
}

int rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                         int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    return group_point_ex(player, group, camera, width, height, out, NULL, clear);
}

static int select_best_target_point(const RR_PLAYER* player, int group, int locked_bone,
                                    const RR_CAMERA* camera, int width, int height,
                                    RR_V3* out, int* out_bone, int (*clear)(RR_V3, RR_V3)) {
    if (out_bone) *out_bone = -1;

    /* Keep the previously locked bone while it remains valid and clear so
     * multi-joint groups (e.g. Closest / Arms) do not oscillate left/right. */
    if (locked_bone >= 0 && specific_joint_point(player, locked_bone, out)) {
        if (!clear || (camera && clear(camera->pos, *out))) {
            if (out_bone) *out_bone = locked_bone;
            return 1;
        }
    }

    if (!clear || !camera) {
        return group_point_ex(player, group, camera, width, height, out, out_bone, NULL);
    }

    RR_V3 candidate;
    int bone = -1;
    if (group_point_ex(player, group, camera, width, height, &candidate, &bone, clear) &&
        clear(camera->pos, candidate)) {
        *out = candidate;
        if (out_bone) *out_bone = bone;
        return 1;
    }

    static const int fallback_order[] = {
        RR_AIM_HEAD, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS
    };
    for (int i = 0; i < 4; ++i) {
        int fb = fallback_order[i];
        if (fb == group) continue;
        bone = -1;
        if (group_point_ex(player, fb, camera, width, height, &candidate, &bone, clear) &&
            clear(camera->pos, candidate)) {
            *out = candidate;
            if (out_bone) *out_bone = bone;
            return 1;
        }
    }
    return 0;
}

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                       int width, int height, const struct BR_Config* config,
                       uint64_t locked_addr, int32_t locked_id, int locked_bone,
                       int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = { 0 };
    best.bone = -1;
    if (!players || count <= 0 || !camera || !camera->valid || !config || width <= 0 || height <= 0)
        return best;

    float cx = (float)width * 0.5f, cy = (float)height * 0.5f;
    float best_score = 1e30f;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->valid || p->local || p->dead || p->is_vehicle || p->is_loot || p->hidden_in_object)
            continue;
        if (!rr_relationship_allowed(p->relationships, config->aa_target_clan, config->aa_target_police))
            continue;
        if (config->party_filter && rr_party_is_member(p->name, p->pos.x, p->pos.y, p->pos.z))
            continue;
        if (config->aa_dist > 0 && p->dist > (float)config->aa_dist / 100.0f)
            continue;

        int is_locked = locked_addr && p->addr == locked_addr && p->id == locked_id;
        if (locked_addr && !is_locked) continue;

        RR_V3 pt;
        int chosen_bone = -1;
        float sx, sy;
        if (!select_best_target_point(p, config->aa_bone, is_locked ? locked_bone : -1,
                                      camera, width, height, &pt, &chosen_bone, clear))
            continue;
        if (!game_w2s(camera, &pt, width, height, &sx, &sy)) continue;

        float dx = sx - cx, dy = sy - cy;
        float ring = hypotf(dx, dy);
        if (!is_locked && ring > (float)config->aa_fov_px) continue;

        float score = config->aa_closest_first ? p->dist : ring;
        if (score < best_score) {
            best_score = score;
            best.player = p;
            best.point = pt;
            best.dx = dx;
            best.dy = dy;
            best.bone = chosen_bone;
        }
    }
    return best;
}

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing, int* mx, int* my) {
    RR_AIM_STEER local = { 0 };
    if (!state) state = &local;
    if (mx) *mx = 0;
    if (my) *my = 0;
    int out_x = aim_axis(dx, fov, height, smooth, cal, elapsed_ms, firing, &state->carry_x);
    int out_y = aim_axis(dy, fov, height, smooth, cal, elapsed_ms, firing, &state->carry_y);
    if (mx) *mx = out_x;
    if (my) *my = out_y;
}

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth, RR_V3* out) {
    if (!out || sample_ms < 8 || sample_ms > 200 || age_ms > 90) return 0;

    float dt = (float)sample_ms * 0.001f;
    float vx = (position.x - previous.x) / dt;
    float vy = (position.y - previous.y) / dt;
    float vz = (position.z - previous.z) / dt;

    if (vy > 14.0f) vy = 14.0f;
    if (vy < -22.0f) vy = -22.0f;

    float speed = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(speed) || speed < 0.25f || speed > 38.0f) return 0;

    if (smooth < 1) smooth = 1;
    if (smooth > 30) smooth = 30;
    float base_ms = (smooth <= 1) ? 0.0f : (11.0f + (float)(smooth - 1) * 8.5f);
    float lead = ((float)age_ms + base_ms * 0.42f) * 0.001f;
    if (lead > 0.090f) lead = 0.090f;

    *out = (RR_V3){
        point.x + vx * lead,
        point.y + vy * lead,
        point.z + vz * lead
    };
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}