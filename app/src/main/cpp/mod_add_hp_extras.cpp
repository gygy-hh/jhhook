#include "mod_add_hp_extras.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "counter_hooks.hpp"
#include "counter_state.hpp"
#include "game_hooks.hpp"
#include "jh_abi.hpp"
#include "mod_confusion.hpp"

#define AH_LOG(prio, ...) __android_log_print(prio, "CounterAttackMod", __VA_ARGS__)
#define GM_LOG(prio, ...) __android_log_print(prio, "GameModd", __VA_ARGS__)
#define MS_LOG(prio, ...) __android_log_print(prio, "ModStateManager", __VA_ARGS__)

namespace mod_add_hp_i {

// --- libnative `LooksLikeGameHeapPtr`
inline bool looks_like(uintptr_t p) {
    return p != 0 && p >= 0x1000u && (p & 7u) == 0;
}

// --- EngineStringReleaseUserPtr（libnative sub_AB624）
void engine_string_release_user_ptr(void *user_ptr) {
    if (!user_ptr || !GameHooks::fn_engine_ctrl_release)
        return;
    const uintptr_t up = reinterpret_cast<uintptr_t>(user_ptr);
    if ((up & 7u) != 0)
        return;
    char *root = reinterpret_cast<char *>(user_ptr) - 24;
    if ((reinterpret_cast<uintptr_t>(root) & 7u) != 0)
        return;
    if (reinterpret_cast<uintptr_t>(root) == GameHooks::addr_engine_string_empty_control_block)
        return;
    GameHooks::fn_engine_ctrl_release(root);
}

void *node_get_child_by_engine_name(void *node, const char *utf8_z) {
    if (!node || !utf8_z || !looks_like(reinterpret_cast<uintptr_t>(node)))
        return nullptr;
    if (!GameHooks::fn_sub_engine_string_from_utf8 || !GameHooks::fn_node_get_child_by_name)
        return nullptr;
    void *engine_ref = nullptr;
    GameHooks::fn_sub_engine_string_from_utf8(&engine_ref, utf8_z);
    if (!engine_ref)
        return nullptr;
    void *ch = GameHooks::fn_node_get_child_by_name(node, &engine_ref);
    engine_string_release_user_ptr(engine_ref);
    if (!ch || !looks_like(reinterpret_cast<uintptr_t>(ch)))
        return nullptr;
    return ch;
}

void refresh_battle_head_portrait_from_png_id(void *battle_head_ptr, int png_id, bool through_slot_97,
        bool verbose_log);

void rescue_near_zero_scale_x(void *node) {
    if (!node || !looks_like(reinterpret_cast<uintptr_t>(node)))
        return;
    if (!GameHooks::fn_get_scale_x || !GameHooks::fn_set_scale_x)
        return;
    const float sx = GameHooks::fn_get_scale_x(node);
    if (sx > -0.02f && sx < 0.02f)
        GameHooks::fn_set_scale_x(node, 1.f);
}

void restore_portrait_node_material_only(void *node) {
    if (!node || !looks_like(reinterpret_cast<uintptr_t>(node)))
        return;
    if (GameHooks::fn_node_set_cascade_opacity_enabled)
        GameHooks::fn_node_set_cascade_opacity_enabled(node, false);
    if (GameHooks::fn_node_set_opacity)
        GameHooks::fn_node_set_opacity(node, 255);
    static const unsigned char k_white_rgb[3] = {255, 255, 255};
    if (GameHooks::fn_node_set_color_rgb)
        GameHooks::fn_node_set_color_rgb(node, k_white_rgb);
}

/// libnative `RestorePortraitNodeChainBelowCsbRoot`
void restore_portrait_node_chain_below_csb_root(void *portrait_node, void *csb_root) {
    if (!portrait_node || !csb_root ||
            !looks_like(reinterpret_cast<uintptr_t>(portrait_node)) ||
            !looks_like(reinterpret_cast<uintptr_t>(csb_root)))
        return;
    void *walk = portrait_node;
    for (int depth = 0;
            depth < 16 && walk && looks_like(reinterpret_cast<uintptr_t>(walk));
            ++depth) {
        if (walk == csb_root)
            break;
        restore_portrait_node_material_only(walk);
        // 仅叶子肖像节点 setVisible，避免沿 parent 链把兄弟 Buff/UI 整棵树拉出可见（闪烁）。
        if (depth == 0 && GameHooks::fn_node_set_visible)
            GameHooks::fn_node_set_visible(walk, 1);
        rescue_near_zero_scale_x(walk);
        if (!GameHooks::fn_node_get_parent)
            break;
        walk = GameHooks::fn_node_get_parent(walk);
    }
}

void push_unique(std::vector<void *> *vec, void *p) {
    if (!p || !looks_like(reinterpret_cast<uintptr_t>(p)) || !vec)
        return;
    if (std::find(vec->begin(), vec->end(), p) != vec->end())
        return;
    vec->push_back(p);
}

static const char *kCharChar1[] = {"char", "char1"};
static const char *kHpLabelLayers[] = {"char", "char1", "char", "char1", "char", "char1", "ui_bg12_1"};
static const char *kNameTextLayers[] = {"char", "char1", "char", "char1", "ui_bg12_1"};
static const char *kHudOuter[] = {"char", "char1", "ui_bg12_1"};
static const char *kHudInner[] = {"ui_bg12_1"};

void collect_from_char_like_parents(void *csb_root, std::vector<void *> *out_vec) {
    if (!csb_root || !out_vec)
        return;
    for (const char *layer : kCharChar1) {
        void *layer_node = node_get_child_by_engine_name(csb_root, layer);
        if (!layer_node)
            continue;
        void *head_node = node_get_child_by_engine_name(layer_node, "head");
        if (!head_node)
            continue;
        push_unique(out_vec, head_node);
    }
}

void collect_head_portrait_nodes(void *csb, std::vector<void *> *acc) {
    if (!csb || !acc)
        return;
    collect_from_char_like_parents(csb, acc);
    if (!acc->empty())
        return;
    void *root_head = node_get_child_by_engine_name(csb, "head");
    if (root_head)
        push_unique(acc, root_head);
}

void *battle_head_csb_find_hp_label(void *csb) {
    auto *n = reinterpret_cast<void **>(csb);
    if (!n || !looks_like(reinterpret_cast<uintptr_t>(n)))
        return nullptr;
    for (const char *layer : kHpLabelLayers) {
        void *layer_node = node_get_child_by_engine_name(n, layer);
        if (!layer_node)
            continue;
        void *num = node_get_child_by_engine_name(layer_node, "num");
        if (num)
            return num;
    }
    return nullptr;
}

void *battle_head_csb_find_name_text(void *csb) {
    auto *n = reinterpret_cast<void **>(csb);
    if (!n || !looks_like(reinterpret_cast<uintptr_t>(n)))
        return nullptr;
    for (const char *layer : kNameTextLayers) {
        void *layer_node = node_get_child_by_engine_name(n, layer);
        if (!layer_node)
            continue;
        void *t = node_get_child_by_engine_name(layer_node, "Text_1");
        if (t)
            return t;
    }
    return nullptr;
}

void collect_buff_slots(void *csb_root, std::vector<void *> *acc) {
    if (!csb_root || !acc)
        return;
    auto *base = reinterpret_cast<char *>(csb_root);
    for (int i = 91; i <= 93; ++i) {
        void *slot = *reinterpret_cast<void **>(base + static_cast<size_t>(i) * 8u);
        push_unique(acc, slot);
    }
}

void collect_hp_bar_decoration(void *csb_root, std::vector<void *> *acc) {
    if (!csb_root || !acc)
        return;
    auto *n = reinterpret_cast<void **>(csb_root);
    for (const char *outer : kHudOuter) {
        void *layer = node_get_child_by_engine_name(n, outer);
        if (!layer)
            continue;
        for (const char *inner : kHudInner) {
            void *inner_node = node_get_child_by_engine_name(layer, inner);
            if (inner_node)
                push_unique(acc, inner_node);
        }
    }
}

void collect_buff_and_hp_hud(void *csb, std::vector<void *> *acc) {
    collect_buff_slots(csb, acc);
    collect_hp_bar_decoration(csb, acc);
}

void run_die_fade_sequence_on_node(void *node) {
    if (!node || !looks_like(reinterpret_cast<uintptr_t>(node)))
        return;
    if (!GameHooks::fn_delay_time_create || !GameHooks::fn_fade_out_create ||
        !GameHooks::fn_sequence_two_actions || !GameHooks::fn_node_run_action)
        return;
    void *delay = GameHooks::fn_delay_time_create(0.05f);
    void *fade = GameHooks::fn_fade_out_create(nullptr, 0.5f);
    if (!delay || !fade)
        return;
    void *seq = GameHooks::fn_sequence_two_actions(delay, fade);
    if (seq)
        GameHooks::fn_node_run_action(node, seq);
}

void force_hide_battle_head_portrait_for_dead(void *battle_head) {
    if (!battle_head || !looks_like(reinterpret_cast<uintptr_t>(battle_head)))
        return;
    if (!GameHooks::fn_node_set_visible)
        return;
    GameHooks::fn_node_set_visible(battle_head, 0);
    auto *base = reinterpret_cast<char *>(battle_head);
    for (int slot = 94; slot <= 97; ++slot) {
        void *child = *reinterpret_cast<void **>(base + static_cast<size_t>(slot) * 8u);
        if (child && looks_like(reinterpret_cast<uintptr_t>(child)))
            GameHooks::fn_node_set_visible(child, 0);
    }
}

pthread_mutex_t g_derivative_schedule_mu = PTHREAD_MUTEX_INITIALIZER;
void *g_pending_derivative_portrait_player = nullptr;
int g_pending_derivative_skips_remaining = 0;

/// libnative `g_derivativePortraitShouJiTimelineHoldPlayers`：`beenAttack` 后加入，供 drawScene 内 ShouJi 轨刷新。
std::unordered_set<void *> g_derivative_shouji_timeline_hold_players;

/// 对齐 InitializeHooks：`g_derivativePortraitDrawSceneWarmupRemaining = 2`
std::atomic<int> g_derivative_flush_draw_scene_warmup{2};

void schedule_derivative_portrait_refresh(void *jh_player) {
    if (!jh_player)
        return;
    pthread_mutex_lock(&g_derivative_schedule_mu);
    g_pending_derivative_portrait_player = jh_player;
    g_pending_derivative_skips_remaining = 3;
    pthread_mutex_unlock(&g_derivative_schedule_mu);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] ScheduleDerivativePortraitRefresh pending=%p skips=3",
            jh_player);
}

void schedule_derivative_portrait_refresh_soon(void *jh_player) {
    if (!jh_player)
        return;
    pthread_mutex_lock(&g_derivative_schedule_mu);
    const bool same = g_pending_derivative_portrait_player == jh_player;
    const bool keep_skip = same && g_pending_derivative_skips_remaining != 0;
    g_pending_derivative_portrait_player = jh_player;
    g_pending_derivative_skips_remaining = 0;
    if (keep_skip)
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] ScheduleDerivativePortraitRefreshSoon pending=%p",
                jh_player);
    pthread_mutex_unlock(&g_derivative_schedule_mu);
}

/// 模板人偶 id；区分型号用 cfg personality（jh_player_personality_id → cfg+136）。
static constexpr uint32_t kDerivativePersonTemplateId = 296u;
/// cfg+120 → `res_head/head%d.png`（朱煞 / 青影 / 金煌 分立绘 id）。
static constexpr uint32_t kPortraitJinHuang = 202605101u;
static constexpr uint32_t kPortraitQingYing = 202605102u;
static constexpr uint32_t kPortraitZhusha = 202605103u;
static constexpr uint32_t kDerivativePersZhusha = 101u;
static constexpr uint32_t kDerivativePersQingYing = 102u;
static constexpr uint32_t kDerivativePersJinHuang = 103u;

bool is_derivative_transformed_battle_player(void *jh_player) {
    void *person = jh_player_person(jh_player);
    if (!jh_player || !person)
        return false;
    if (static_cast<uint32_t>(jh_player_person_id(jh_player)) != kDerivativePersonTemplateId)
        return false;
    const uint32_t pers = jh_player_personality_id(jh_player);
    return pers == kDerivativePersZhusha || pers == kDerivativePersQingYing || pers == kDerivativePersJinHuang;
}

bool is_qingying_derivative(void *jh_player) {
    return jh_player && looks_like(reinterpret_cast<uintptr_t>(jh_player)) &&
           static_cast<uint32_t>(jh_player_person_id(jh_player)) == kDerivativePersonTemplateId &&
           jh_player_personality_id(jh_player) == kDerivativePersQingYing;
}

bool is_jinhuang_derivative(void *jh_player) {
    return jh_player && looks_like(reinterpret_cast<uintptr_t>(jh_player)) &&
           static_cast<uint32_t>(jh_player_person_id(jh_player)) == kDerivativePersonTemplateId &&
           jh_player_personality_id(jh_player) == kDerivativePersJinHuang;
}

pthread_mutex_t g_jinhuang_skip_mu = PTHREAD_MUTEX_INITIALIZER;
std::unordered_set<uintptr_t> g_jinhuang_skip_next_begin_act_once;

pthread_mutex_t g_qingying_pair_mu = PTHREAD_MUTEX_INITIALIZER;
std::set<std::pair<uintptr_t, uintptr_t>> g_qingying_first_damage_pairs_round;

void derivative_act_flags_on_new_fight_round_body() {
    pthread_mutex_lock(&g_jinhuang_skip_mu);
    g_jinhuang_skip_next_begin_act_once.clear();
    pthread_mutex_unlock(&g_jinhuang_skip_mu);

    pthread_mutex_lock(&g_qingying_pair_mu);
    g_qingying_first_damage_pairs_round.clear();
    pthread_mutex_unlock(&g_qingying_pair_mu);
}

void maybe_derivative_qingying_counter_on_damage_body(void *attacker_jh, void *vic_eff_jh, unsigned hp_u) {
    if (!attacker_jh || !vic_eff_jh || vic_eff_jh == attacker_jh || (hp_u & 0x80000000u) == 0u)
        return;

    const bool confused_hit_ally = ModConfusion::is_active_for_player(attacker_jh) &&
                                   jh_player_same_side(attacker_jh, vic_eff_jh);
    if (confused_hit_ally)
        return;

    if (!is_qingying_derivative(vic_eff_jh))
        return;

    const uintptr_t pa = reinterpret_cast<uintptr_t>(attacker_jh);
    const uintptr_t pv = reinterpret_cast<uintptr_t>(vic_eff_jh);
    pthread_mutex_lock(&g_qingying_pair_mu);
    const bool first_hit =
            g_qingying_first_damage_pairs_round.insert(std::make_pair(pa, pv)).second;
    pthread_mutex_unlock(&g_qingying_pair_mu);
    if (!first_hit)
        return;

    const int roll = (std::rand() % 100) + 1;
    if (roll > 50) {
        GM_LOG(ANDROID_LOG_ERROR,
                "[QingYingCounter] rollFail vicPid=%d roll=%d thr=50 atkPid=%d",
                jh_player_person_id(vic_eff_jh),
                roll,
                jh_player_person_id(attacker_jh));
        return;
    }

    GM_LOG(ANDROID_LOG_ERROR,
            "[QingYingCounter] trigger vicPid=%d roll=%d atkPid=%d",
            jh_player_person_id(vic_eff_jh),
            roll,
            jh_player_person_id(attacker_jh));

    const uint32_t atk_skill = jh_player_person_skill_id_u32(attacker_jh);
    if (atk_skill != 0u) {
        (void) CounterState::try_record_original_skill(vic_eff_jh);
        jh_player_set_person_skill_id_u32(vic_eff_jh, atk_skill);
        void *cached = jh_player_cached_skill(vic_eff_jh);
        if (cached && GameHooks::fn_skill_init_for_player)
            (void) GameHooks::fn_skill_init_for_player(cached, vic_eff_jh, atk_skill);
        if (GameHooks::fn_get_battle_head && GameHooks::fn_battle_head_change_skill) {
            void *head = GameHooks::fn_get_battle_head(vic_eff_jh);
            if (head)
                GameHooks::fn_battle_head_change_skill(head, atk_skill);
        }
        GM_LOG(ANDROID_LOG_ERROR,
                "[QingYingCounter] skillMirror vicPid=%d atkSkill=%u",
                jh_player_person_id(vic_eff_jh),
                atk_skill);
    }

    CounterState::mark_counter_attack_action(vic_eff_jh);

    void *mgr = jh_player_battle_mgr(vic_eff_jh);
    if (mgr && GameHooks::fn_fight_insert) {
        GameHooks::fn_fight_insert(mgr, vic_eff_jh);
        GM_LOG(ANDROID_LOG_ERROR,
                "[QingYingCounter] fightInsert vicPid=%d",
                jh_player_person_id(vic_eff_jh));
    }
}

/// 对齐 lst：`JhPlayer::getPower` 功力链 `0xFC + 0x130 + 0x15C`；`getSpeed` 速度链 `0x128 + 0x160`。
static constexpr size_t kJhOffBasePower = 0xFCu;
static constexpr size_t kJhOffPowerMid = 0x130u;
static constexpr size_t kJhOffPowerBonus = 0x15Cu;
static constexpr size_t kJhOffBaseSpeed = 0x128u;
static constexpr size_t kJhOffSpeedBonus = 0x160u;

struct ZhushaAmpRecord {
    int32_t delta_power_15c{};
    int32_t delta_speed_160{};
    int expire_exclusive_round{};
};

pthread_mutex_t g_zhusha_amp_mu = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<uintptr_t, ZhushaAmpRecord> g_zhusha_amp_by_jh;
static int g_zhusha_amp_tick_last_round = -999999;

static int32_t zhusha_scaled_half_bonus(int64_t sum_components) {
    if (sum_components <= 0)
        return 1;
    int64_t d = sum_components * 50 / 100;
    if (d < 1)
        d = 1;
    if (d > 2147483647LL)
        d = 2147483647LL;
    return static_cast<int32_t>(d);
}

static void zhusha_amp_restore_if_live(void *jh_player, const ZhushaAmpRecord &rec) {
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;
    auto *p15c = reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffPowerBonus);
    auto *p160 = reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffSpeedBonus);
    *p15c -= rec.delta_power_15c;
    *p160 -= rec.delta_speed_160;
    GM_LOG(ANDROID_LOG_ERROR,
            "[ZhushaAmp] restore jh=%p dPow=%d dSpd=%d",
            jh_player,
            rec.delta_power_15c,
            rec.delta_speed_160);
}

void zhusha_amp_clear_all_on_battle_end_body() {
    pthread_mutex_lock(&g_zhusha_amp_mu);
    for (auto &kv : g_zhusha_amp_by_jh)
        zhusha_amp_restore_if_live(reinterpret_cast<void *>(kv.first), kv.second);
    g_zhusha_amp_by_jh.clear();
    pthread_mutex_unlock(&g_zhusha_amp_mu);
    g_zhusha_amp_tick_last_round = -999999;
}

void zhusha_amp_on_new_fight_round_body(int round_param) {
    if (round_param < 0)
        return;
    pthread_mutex_lock(&g_zhusha_amp_mu);
    if (round_param == g_zhusha_amp_tick_last_round) {
        pthread_mutex_unlock(&g_zhusha_amp_mu);
        return;
    }
    g_zhusha_amp_tick_last_round = round_param;

    for (auto it = g_zhusha_amp_by_jh.begin(); it != g_zhusha_amp_by_jh.end(); ) {
        if (round_param >= it->second.expire_exclusive_round) {
            zhusha_amp_restore_if_live(reinterpret_cast<void *>(it->first), it->second);
            it = g_zhusha_amp_by_jh.erase(it);
        } else {
            ++it;
        }
    }
    pthread_mutex_unlock(&g_zhusha_amp_mu);
}

void apply_zhusha_entry_amp_body(void *jh_player) {
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;

    const uintptr_t key = reinterpret_cast<uintptr_t>(jh_player);
    const int expire_ex = ModConfusion::fight_round_snapshot() + 3;

    pthread_mutex_lock(&g_zhusha_amp_mu);
    const auto old_it = g_zhusha_amp_by_jh.find(key);
    if (old_it != g_zhusha_amp_by_jh.end()) {
        zhusha_amp_restore_if_live(jh_player, old_it->second);
        g_zhusha_amp_by_jh.erase(old_it);
    }
    pthread_mutex_unlock(&g_zhusha_amp_mu);

    const int32_t fc =
            *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffBasePower);
    const int32_t v130 =
            *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffPowerMid);
    int32_t v15c =
            *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffPowerBonus);
    const int32_t bs =
            *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffBaseSpeed);
    int32_t v160 =
            *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffSpeedBonus);

    const int64_t sum_pow = static_cast<int64_t>(fc) + static_cast<int64_t>(v130) + v15c;
    const int64_t sum_sp = static_cast<int64_t>(bs) + v160;

    const int32_t d_pow = zhusha_scaled_half_bonus(sum_pow);
    const int32_t d_spd = zhusha_scaled_half_bonus(sum_sp);

    v15c += d_pow;
    v160 += d_spd;
    *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffPowerBonus) = v15c;
    *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + kJhOffSpeedBonus) = v160;

    pthread_mutex_lock(&g_zhusha_amp_mu);
    g_zhusha_amp_by_jh[key] = ZhushaAmpRecord{d_pow, d_spd, expire_ex};
    pthread_mutex_unlock(&g_zhusha_amp_mu);

    GM_LOG(ANDROID_LOG_ERROR,
            "[ZhushaAmp] entry jh=%p expireExclusive=%d dPow=%d dSpd=%d sumPow=%lld sumSpd=%lld",
            jh_player,
            expire_ex,
            d_pow,
            d_spd,
            static_cast<long long>(sum_pow),
            static_cast<long long>(sum_sp));
}

void maybe_derivative_jinhuang_extra_turn_after_begin_act_body(void *jh_owner) {
    if (!jh_owner || !looks_like(reinterpret_cast<uintptr_t>(jh_owner)))
        return;
    if (!is_jinhuang_derivative(jh_owner))
        return;
    if (jh_player_hp_s32(jh_owner) < 1)
        return;

    const uintptr_t k = reinterpret_cast<uintptr_t>(jh_owner);
    pthread_mutex_lock(&g_jinhuang_skip_mu);
    const bool consumed_skip = g_jinhuang_skip_next_begin_act_once.erase(k) != 0u;
    pthread_mutex_unlock(&g_jinhuang_skip_mu);
    if (consumed_skip)
        return;

    const int roll = (std::rand() % 100) + 1;
    if (roll > 70) {
        GM_LOG(ANDROID_LOG_ERROR,
                "[JinHuangExtra] rollFail pid=%d roll=%d thr=70",
                jh_player_person_id(jh_owner),
                roll);
        return;
    }

    void *mgr = jh_player_battle_mgr(jh_owner);
    if (!mgr || !GameHooks::fn_fight_insert)
        return;

    pthread_mutex_lock(&g_jinhuang_skip_mu);
    g_jinhuang_skip_next_begin_act_once.insert(k);
    pthread_mutex_unlock(&g_jinhuang_skip_mu);

    GameHooks::fn_fight_insert(mgr, jh_owner);
    GM_LOG(ANDROID_LOG_ERROR,
            "[JinHuangExtra] fightInsert pid=%d roll=%d",
            jh_player_person_id(jh_owner),
            roll);
}

void sub_a4ce4_maybe_refresh_derivative_portrait(void *victim_jh, int hp_amt) {
    if (!victim_jh || hp_amt >= 0)
        return;
    if (!is_derivative_transformed_battle_player(victim_jh))
        return;
    schedule_derivative_portrait_refresh_soon(victim_jh);
}

struct DerivativePreSnap {
    uint32_t old_cfg120{};
    uint32_t old_cfg136{};
    uintptr_t nick_engine_ref{};
};

pthread_mutex_t g_derivative_restore_mu = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<uintptr_t, DerivativePreSnap> g_deriv_restore_by_jh;

bool snapshot_person_tr_nick_ref(uintptr_t cfg, uintptr_t *out_nick) {
    *out_nick = 0;
    if (cfg <= 0x10000 || !GameHooks::fn_snapshot_person_tr_nick)
        return false;
    uintptr_t slot = 0;
    GameHooks::fn_snapshot_person_tr_nick(reinterpret_cast<void *>(&slot), cfg + 16);
    *out_nick = slot;
    return slot != 0;
}

bool apply_person_tr_display_utf8(uintptr_t cfg, const char *utf8_z) {
    if (cfg <= 0x10000 || !utf8_z || !GameHooks::fn_sub_engine_string_from_utf8 ||
        !GameHooks::fn_person_cfg_assign_display_from_engine_str)
        return false;
    void *engine_ref = nullptr;
    GameHooks::fn_sub_engine_string_from_utf8(&engine_ref, utf8_z);
    if (!engine_ref)
        return false;
    GameHooks::fn_person_cfg_assign_display_from_engine_str(cfg + 16, &engine_ref);
    engine_string_release_user_ptr(engine_ref);
    return true;
}

static const char kZhushaUtf8[] = "\xe6\x9c\xb1\xe7\x85\x9e";
static const char kQingYingUtf8[] = "\xe9\x9d\x92\xe5\xbd\xb1";
static const char kJinHuangUtf8[] = "\xe9\x87\x91\xe7\x85\x8c";

using PersonMgrGetInstanceFn = void *(*)();
using PersonMgrRegisterFn = void *(*)(void *mgr, void *jh_or_person);

void *resolve_person_manager_get_instance() {
    static PersonMgrGetInstanceFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fn = reinterpret_cast<PersonMgrGetInstanceFn>(
                dlsym(RTLD_DEFAULT, "_ZN14PersonManager11GetInstanceEv"));
        if (!fn)
            fn = reinterpret_cast<PersonMgrGetInstanceFn>(
                    dlsym(RTLD_DEFAULT, "_ZN13PersonManager11GetInstanceEv"));
    }
    return fn ? fn() : nullptr;
}

PersonMgrRegisterFn resolve_person_manager_register() {
    static PersonMgrRegisterFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fn = reinterpret_cast<PersonMgrRegisterFn>(
                dlsym(RTLD_DEFAULT, "_ZN14PersonManager14RegisterPersonEP8JhPerson"));
        if (!fn)
            fn = reinterpret_cast<PersonMgrRegisterFn>(
                    dlsym(RTLD_DEFAULT, "_ZN13PersonManager14RegisterPersonEP8JhPerson"));
    }
    return fn;
}

void register_person_best_effort(void *jh_player) {
    void *mgr = resolve_person_manager_get_instance();
    PersonMgrRegisterFn reg = resolve_person_manager_register();
    if (mgr && reg)
        reg(mgr, jh_player);
    else
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] PersonManager dlsym missing mgr=%p reg=%p", mgr,
                reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(reg)));
}

pthread_mutex_t g_hurt_dr_mu = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<int, void *> g_pid_to_battle_mgr_for_hurt_dr;

void reset_hurt_dr_modifier_impl(void *victim_jh);
void prepare_battle_head_fu_huo_visible(void *battle_head_ptr);

void notify_battle_changed_for_hurt_dr(void *victim_jh) {
    if (!victim_jh)
        return;
    const int pid = jh_player_person_id(victim_jh);
    if (pid < 0 || (static_cast<unsigned>(pid) & 0x80000000u) != 0u)
        return;
    void *mgr_now = jh_player_battle_mgr(victim_jh);
    pthread_mutex_lock(&g_hurt_dr_mu);
    auto it = g_pid_to_battle_mgr_for_hurt_dr.find(pid);
    void *mgr_old = nullptr;
    bool had = false;
    if (it != g_pid_to_battle_mgr_for_hurt_dr.end()) {
        had = true;
        mgr_old = it->second;
    }
    const bool changed = had && mgr_old != mgr_now;
    g_pid_to_battle_mgr_for_hurt_dr[pid] = mgr_now;
    pthread_mutex_unlock(&g_hurt_dr_mu);

    if (changed) {
        reset_hurt_dr_modifier_impl(victim_jh);
        MS_LOG(ANDROID_LOG_ERROR, "[HurtDR] battleMgrChanged pid=%d old=%p new=%p", pid, mgr_old, mgr_now);
    }
}

void reset_hurt_dr_modifier_impl(void *victim_jh) {
    const uint32_t pers = jh_player_personality_id(victim_jh);
    const uint32_t nei = jh_player_person_neigong_id_u32(victim_jh);
    if (!(pers == 51u || nei == 3897u))
        return;
    const int ms = jh_player_mianshang_modifier_s32(victim_jh);
    if (ms == 0)
        return;
    jh_player_set_mianshang_modifier_s32(victim_jh, 0);
    GM_LOG(ANDROID_LOG_ERROR, "[ResetHurtDRModifier] pid=%d pers=%u neigong=%u clearedMs=%d",
            jh_player_person_id(victim_jh), pers, nei, ms);
}

int pick_min_int(int a, int b) {
    return (a < b) ? a : b;
}

int pick_max_int(int a, int b) {
    return (a < b) ? b : a;
}

void apply_tian_xing_51(void *victim_jh) {
    if (!victim_jh)
        return;
    const uint32_t pers = jh_player_personality_id(victim_jh);
    const uint32_t nei = jh_player_person_neigong_id_u32(victim_jh);
    if (!(pers == 51u || nei == 3897u))
        return;

    int max_hp = static_cast<int>(jh_player_max_hp_u32(victim_jh));
    if (max_hp <= 0)
        return;

    uint32_t hp_u = jh_player_hp_u32(victim_jh);
    int hp = static_cast<int>(hp_u);
    if ((hp_u & 0x80000000u) != 0u)
        hp = 0;

    const int numer = 100 * (max_hp - hp);
    const int lost_pct = numer / max_hp;
    const int cap28 = 28;
    const int new_ms = pick_min_int(cap28, lost_pct);

    if (nei == 3897u) {
        const int cur = jh_player_mianshang_modifier_s32(victim_jh);
        if (new_ms > cur) {
            jh_player_set_mianshang_modifier_s32(victim_jh, new_ms);
            GM_LOG(ANDROID_LOG_ERROR,
                    "[TianXingDR3897] pers=%u neigong=%u hp=%d max=%u lostPct=%d newMs=%d oldMs=%d", pers, nei, hp,
                    static_cast<unsigned>(max_hp), lost_pct, new_ms, cur);
        }
    } else {
        jh_player_set_mianshang_modifier_s32(victim_jh, new_ms);
        GM_LOG(ANDROID_LOG_ERROR, "[TianXingDR51] pers=%u neigong=%u hp=%d max=%u lostPct=%d newMs=%d", pers, nei, hp,
                static_cast<unsigned>(max_hp), lost_pct, new_ms);
    }
}

void trace_wugong_chaos_3001(void *attacker,
        void *vic_eff,
        void *vic_raw,
        unsigned hp_u,
        int hp_before,
        int hp_after,
        int dmg_type,
        bool add_hp_arg_a5,
        bool add_hp_arg_a6) {
    if (!attacker || (hp_u & 0x80000000u) == 0u)
        return;
    if (jh_player_person_skill_id_u32(attacker) != 3001u)
        return;

    const bool enemy_line = vic_eff != attacker && !jh_player_same_side(attacker, vic_eff);
    const bool tls_match = counter_hooks_tls_skill_actor_ptr() == attacker;
    const bool hp_drop_alive = hp_before > hp_after && hp_after > 0;

    const char *reason = "SKIP";
    int roll = -1;

    if (enemy_line && tls_match && hp_drop_alive) {
        roll = (std::rand() % 100) + 1;
        if (roll <= 75) {
            ModConfusion::apply_overwrite(vic_eff, 2);
            reason = "APPLY_CONFUSE";
            AH_LOG(ANDROID_LOG_ERROR, "[TraceWugongChaos] skill=3001 roll=%d thr=75 vicPid=%d atkPid=%d", roll,
                    jh_player_person_id(vic_eff), jh_player_person_id(attacker));
        } else {
            reason = "SKIP_rollFail";
        }
    } else if (!enemy_line) {
        reason = "SKIP_notEnemyLine";
    } else if (!tls_match) {
        reason = "SKIP_tlsMismatch";
    } else {
        reason = "SKIP_noHpLossOrDead";
    }

    void *tls_actor = counter_hooks_tls_skill_actor_ptr();
    const int tls_pid = tls_actor ? jh_player_person_id(tls_actor) : -1;
    const int vic_raw_pid = vic_raw ? jh_player_person_id(vic_raw) : -1;

    const int round_ev = CounterState::current_round_for_trace();
    AH_LOG(ANDROID_LOG_ERROR,
            "[TraceWugongChaos] round=%d dmgType=%d a5=%u a6=%u atk=%d vicRaw=%d vicEff=%d hpAmt=%d hpB=%d hpA=%d tlsPid=%d "
            "gate(enemy=%d tls=%d hp=%d) roll=%d thr=75 out=%s vicModConf=%d atkModConf=%d",
            round_ev,
            dmg_type,
            static_cast<unsigned>(add_hp_arg_a5),
            static_cast<unsigned>(add_hp_arg_a6),
            jh_player_person_id(attacker),
            vic_raw_pid,
            jh_player_person_id(vic_eff),
            static_cast<int>(hp_u),
            hp_before,
            hp_after,
            tls_pid,
            enemy_line ? 1 : 0,
            tls_match ? 1 : 0,
            hp_drop_alive ? 1 : 0,
            roll,
            reason,
            ModConfusion::is_active_for_player(vic_eff) ? 1 : 0,
            ModConfusion::is_active_for_player(attacker) ? 1 : 0);
}

void apply_derivative_on_death(void *jh_player, void *battle_head) {
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] player=%p battleHead=%p", jh_player, battle_head);
    if (!jh_player)
        return;

    const int hp = jh_player_hp_s32(jh_player);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] hp=%d", hp);
    if (hp >= 1)
        return;

    const int pid = jh_player_person_id(jh_player);
    const uint32_t pers = jh_player_personality_id(jh_player);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] personId=%d personality=%u", pid, pers);

    if (static_cast<unsigned>(pid) != 205u || pers != 100u) {
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] skip branch id/personality mismatch");
        return;
    }

    int max_hp = static_cast<int>(jh_player_max_hp_u32(jh_player));
    if (max_hp <= 0)
        max_hp = 1200;
    const int ten = 10;
    const int seventyfive_pct = (75 * max_hp) / 100;
    const int revive_hp = pick_max_int(ten, seventyfive_pct);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] reviveHp calc maxHp=%d -> reviveHp=%d", max_hp, revive_hp);

    void *person = jh_player_person(jh_player);
    uintptr_t cfg = reinterpret_cast<uintptr_t>(jh_person_config_ptr(person));
    if (cfg > 0x10000) {
        DerivativePreSnap snap{};
        snap.old_cfg120 = *reinterpret_cast<uint32_t *>(cfg + 120);
        snap.old_cfg136 = *reinterpret_cast<uint32_t *>(cfg + 136);
        snapshot_person_tr_nick_ref(cfg, &snap.nick_engine_ref);
        pthread_mutex_lock(&g_derivative_restore_mu);
        g_deriv_restore_by_jh[reinterpret_cast<uintptr_t>(jh_player)] = snap;
        pthread_mutex_unlock(&g_derivative_restore_mu);
    }

    jh_player_set_hp_s32(jh_player, revive_hp);
    jh_player_set_revive_flag_s32(jh_player, 1);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] setHp=%d setRevive=1", revive_hp);

    const int spawn_roll = (std::rand() % 100) + 1;
    uint32_t deriv_pers = kDerivativePersZhusha;
    const char *deriv_display = kZhushaUtf8;
    uint32_t portrait_cfg120 = kPortraitZhusha;
    if (spawn_roll > 20 && spawn_roll <= 60) {
        deriv_pers = kDerivativePersQingYing;
        deriv_display = kQingYingUtf8;
        portrait_cfg120 = kPortraitQingYing;
    } else if (spawn_roll > 60) {
        deriv_pers = kDerivativePersJinHuang;
        deriv_display = kJinHuangUtf8;
        portrait_cfg120 = kPortraitJinHuang;
    }
    GM_LOG(ANDROID_LOG_ERROR,
            "[DerivativeDeath] spawnRoll=%d derivPers=%u displayUtf8=%s",
            spawn_roll,
            deriv_pers,
            deriv_display);

    if (person)
        jh_person_set_id_u32(person, kDerivativePersonTemplateId);
    GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] setPersonId=%u", kDerivativePersonTemplateId);

    if (cfg > 0x10000) {
        *reinterpret_cast<uint32_t *>(cfg + 136) = deriv_pers;
        *reinterpret_cast<uint32_t *>(cfg + 120) = portrait_cfg120;
        if (apply_person_tr_display_utf8(cfg, deriv_display))
            GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] ApplyPersonTrDisplayNameUtf8 ok");
        else
            GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] ApplyPersonTrDisplayNameUtf8 fail");
    }

    register_person_best_effort(jh_player);

    if (deriv_pers == kDerivativePersZhusha)
        apply_zhusha_entry_amp_body(jh_player);

    schedule_derivative_portrait_refresh(jh_player);

    if (battle_head && GameHooks::fn_battle_head_change_skill) {
        const uint32_t skid = jh_player_person_skill_id_u32(jh_player);
        GameHooks::fn_battle_head_change_skill(battle_head, skid);
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] changeSkill=%u", skid);
    }

    if (battle_head && GameHooks::fn_battle_head_fu_huo) {
        prepare_battle_head_fu_huo_visible(battle_head);
        GameHooks::fn_battle_head_fu_huo(battle_head, 0.0);
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] fuHuo path");
    } else if (battle_head && GameHooks::fn_battle_head_update_head) {
        GameHooks::fn_battle_head_update_head(battle_head);
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] updateHead path");
    }

    void *mgr = jh_player_battle_mgr(jh_player);
    if (mgr && GameHooks::fn_fight_insert) {
        GameHooks::fn_fight_insert(mgr, jh_player);
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] fightInsert");
    } else {
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativeDeath] fightInsert skip mgr=%p fi=%p", mgr,
                reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(GameHooks::fn_fight_insert)));
    }
}

/// `apply_derivative_on_death` 仅对 pid==205、pers==100 生效；战后必须把快照写回，否则人物界面仍为衍生物。
static constexpr uint32_t kDerivativeBranchOriginalPersonId = 205u;

void restore_derivative_person_from_snap_after_battle(void *jh_player, const DerivativePreSnap &snap) {
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;
    if (!is_derivative_transformed_battle_player(jh_player))
        return;

    void *person = jh_player_person(jh_player);
    if (!person)
        return;
    uintptr_t cfg = reinterpret_cast<uintptr_t>(jh_person_config_ptr(person));
    if (cfg <= 0x10000)
        return;

    *reinterpret_cast<uint32_t *>(cfg + 120) = snap.old_cfg120;
    *reinterpret_cast<uint32_t *>(cfg + 136) = snap.old_cfg136;

    if (snap.nick_engine_ref != 0 && GameHooks::fn_person_cfg_assign_display_from_engine_str) {
        void *eref = reinterpret_cast<void *>(snap.nick_engine_ref);
        GameHooks::fn_person_cfg_assign_display_from_engine_str(cfg + 16, &eref);
        engine_string_release_user_ptr(eref);
    }

    jh_person_set_id_u32(person, kDerivativeBranchOriginalPersonId);
    register_person_best_effort(jh_player);

    if (GameHooks::fn_get_battle_head && snap.old_cfg120 != 0u) {
        void *bh = GameHooks::fn_get_battle_head(jh_player);
        if (bh && looks_like(reinterpret_cast<uintptr_t>(bh)))
            refresh_battle_head_portrait_from_png_id(
                    bh, static_cast<int>(snap.old_cfg120), true, false);
    }

    GM_LOG(ANDROID_LOG_ERROR,
            "[DerivativeRestore] postBattle jh=%p personId->%u cfg120=%u cfg136=%u",
            jh_player,
            kDerivativeBranchOriginalPersonId,
            snap.old_cfg120,
            snap.old_cfg136);
}

pthread_mutex_t g_bh_death_mu = PTHREAD_MUTEX_INITIALIZER;
std::unordered_set<uintptr_t> g_bh_death_cleanup_armed;

void cancel_battle_head_portrait_death_cleanup(void *battle_head) {
    if (!battle_head)
        return;
    pthread_mutex_lock(&g_bh_death_mu);
    g_bh_death_cleanup_armed.erase(reinterpret_cast<uintptr_t>(battle_head));
    pthread_mutex_unlock(&g_bh_death_mu);
}

void prepare_battle_head_fu_huo_visible(void *battle_head_ptr) {
    auto *battle_head = reinterpret_cast<void **>(battle_head_ptr);
    if (!battle_head || !looks_like(reinterpret_cast<uintptr_t>(battle_head)))
        return;
    cancel_battle_head_portrait_death_cleanup(battle_head);

    if (!GameHooks::fn_node_set_visible)
        return;
    GameHooks::fn_node_set_visible(battle_head, 1);
    auto *base = reinterpret_cast<char *>(battle_head);
    for (int slot = 94; slot <= 97; ++slot) {
        void *child = *reinterpret_cast<void **>(base + static_cast<size_t>(slot) * 8u);
        if (child && looks_like(reinterpret_cast<uintptr_t>(child)))
            GameHooks::fn_node_set_visible(child, 1);
    }
    if (!GameHooks::fn_node_set_opacity)
        return;

    for (int slot = 94; slot <= 97; ++slot) {
        void *csb = *reinterpret_cast<void **>(base + static_cast<size_t>(slot) * 8u);
        if (!csb || !looks_like(reinterpret_cast<uintptr_t>(csb)))
            continue;
        std::vector<void *> portrait_nodes;
        collect_head_portrait_nodes(csb, &portrait_nodes);
        for (void *n : portrait_nodes) {
            if (n && looks_like(reinterpret_cast<uintptr_t>(n)))
                GameHooks::fn_node_set_opacity(n, 255);
        }
        void *hp_label = battle_head_csb_find_hp_label(csb);
        if (hp_label)
            GameHooks::fn_node_set_opacity(hp_label, 255);
        void *name_t = battle_head_csb_find_name_text(csb);
        if (name_t)
            GameHooks::fn_node_set_opacity(name_t, 255);

        std::vector<void *> hud;
        collect_buff_and_hp_hud(csb, &hud);
        for (void *n : hud) {
            if (!n || !looks_like(reinterpret_cast<uintptr_t>(n)))
                continue;
            if (GameHooks::fn_node_set_visible)
                GameHooks::fn_node_set_visible(n, 1);
            GameHooks::fn_node_set_opacity(n, 255);
        }
    }
}

void fade_cleanup_inner_slots(void *battle_head_ptr) {
    auto *battle_head = reinterpret_cast<void **>(battle_head_ptr);
    if (!battle_head)
        return;
    auto *base = reinterpret_cast<char *>(battle_head);

    const bool have_actions = GameHooks::fn_delay_time_create && GameHooks::fn_fade_out_create &&
            GameHooks::fn_sequence_two_actions && GameHooks::fn_node_run_action;

    for (int slot = 94; slot <= 97; ++slot) {
        void *csb = *reinterpret_cast<void **>(base + static_cast<size_t>(slot) * 8u);
        if (!csb || !looks_like(reinterpret_cast<uintptr_t>(csb)))
            continue;

        if (!have_actions) {
            force_hide_battle_head_portrait_for_dead(battle_head);
            pthread_mutex_lock(&g_bh_death_mu);
            g_bh_death_cleanup_armed.erase(reinterpret_cast<uintptr_t>(battle_head));
            pthread_mutex_unlock(&g_bh_death_mu);
            return;
        }

        std::vector<void *> portrait_nodes;
        collect_head_portrait_nodes(csb, &portrait_nodes);
        for (void *n : portrait_nodes)
            run_die_fade_sequence_on_node(n);

        void *hp_label = battle_head_csb_find_hp_label(csb);
        if (hp_label)
            run_die_fade_sequence_on_node(hp_label);
        void *name_t = battle_head_csb_find_name_text(csb);
        if (name_t)
            run_die_fade_sequence_on_node(name_t);

        std::vector<void *> hud;
        collect_buff_and_hp_hud(csb, &hud);
        for (void *n : hud)
            run_die_fade_sequence_on_node(n);
    }
}

void begin_battle_head_portrait_death_fade_cleanup(void *battle_head) {
    if (!battle_head || !looks_like(reinterpret_cast<uintptr_t>(battle_head)))
        return;

    pthread_mutex_lock(&g_bh_death_mu);
    const uintptr_t key = reinterpret_cast<uintptr_t>(battle_head);
    const bool already = g_bh_death_cleanup_armed.count(key) != 0;
    if (!already)
        g_bh_death_cleanup_armed.insert(key);
    pthread_mutex_unlock(&g_bh_death_mu);
    if (already)
        return;

    fade_cleanup_inner_slots(battle_head);
}

void refresh_battle_head_portrait_from_png_id(void *battle_head_ptr, int png_id, bool through_slot_97,
        bool verbose_log) {
    if (!battle_head_ptr || png_id <= 0)
        return;
    if (!GameHooks::fn_jh_set_sprite_frame || !GameHooks::fn_sub_engine_string_from_utf8)
        return;
    auto *battle_head = reinterpret_cast<void **>(battle_head_ptr);
    if (!battle_head || !looks_like(reinterpret_cast<uintptr_t>(battle_head)))
        return;

    void **slot720 = reinterpret_cast<void **>(reinterpret_cast<char *>(battle_head_ptr) + 720);
    void *bound_jh = slot720 ? *slot720 : nullptr;
    if (bound_jh && looks_like(reinterpret_cast<uintptr_t>(bound_jh)) &&
            jh_player_hp_s32(bound_jh) <= 0)
        return;

    char buf[112]{};
    (void) std::snprintf(buf, sizeof(buf), "res_head/head%d.png", png_id);
    void *engine_ref = nullptr;
    GameHooks::fn_sub_engine_string_from_utf8(&engine_ref, buf);
    if (!engine_ref) {
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] refresh PNG engine ref fail path=%s", buf);
        return;
    }

    const int slot_end = through_slot_97 ? 97 : 94;
    auto *base = reinterpret_cast<char *>(battle_head);
    int applied = 0;
    for (int slot = 94; slot <= slot_end; ++slot) {
        void *csb = *reinterpret_cast<void **>(base + static_cast<size_t>(slot) * 8u);
        if (!csb || !looks_like(reinterpret_cast<uintptr_t>(csb)))
            continue;
        std::vector<void *> portrait_nodes;
        collect_head_portrait_nodes(csb, &portrait_nodes);
        for (void *node : portrait_nodes) {
            if (!node || !looks_like(reinterpret_cast<uintptr_t>(node)))
                continue;
            GameHooks::fn_jh_set_sprite_frame(node, &engine_ref);
            restore_portrait_node_chain_below_csb_root(node, csb);
            ++applied;
        }
    }
    engine_string_release_user_ptr(engine_ref);
    if (verbose_log)
        GM_LOG(ANDROID_LOG_ERROR,
                "[DerivativePortrait] refreshPlistPng head=%p pngId=%d slots94-%d nodes=%d",
                battle_head_ptr,
                png_id,
                slot_end,
                applied);
}

void set_derivative_shouji_portrait_timeline_hold(void *jh_player) {
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;
    pthread_mutex_lock(&g_derivative_schedule_mu);
    g_derivative_shouji_timeline_hold_players.insert(jh_player);
    pthread_mutex_unlock(&g_derivative_schedule_mu);
}

/// libnative `RefreshDerivativePortraitDuringShouJiTimelineIfNeeded`（简化：直接遍历 hold 集合）。
void refresh_derivative_portrait_during_shouji_timeline_if_needed_body() {
    if (!GameHooks::fn_get_battle_head || !GameHooks::fn_jh_set_sprite_frame ||
            !GameHooks::fn_sub_engine_string_from_utf8)
        return;

    std::vector<void *> snapshot;
    pthread_mutex_lock(&g_derivative_schedule_mu);
    snapshot.assign(g_derivative_shouji_timeline_hold_players.begin(),
            g_derivative_shouji_timeline_hold_players.end());
    pthread_mutex_unlock(&g_derivative_schedule_mu);

    if (snapshot.empty())
        return;

    std::vector<void *> to_remove_from_hold;

    for (void *jh : snapshot) {
        if (!jh || !looks_like(reinterpret_cast<uintptr_t>(jh)))
            continue;

        if (jh_player_hp_s32(jh) <= 0) {
            void *bh_dead = GameHooks::fn_get_battle_head(jh);
            if (bh_dead && looks_like(reinterpret_cast<uintptr_t>(bh_dead)))
                begin_battle_head_portrait_death_fade_cleanup(bh_dead);
            to_remove_from_hold.push_back(jh);
            continue;
        }

        void *battle_head = GameHooks::fn_get_battle_head(jh);
        if (!battle_head || !looks_like(reinterpret_cast<uintptr_t>(battle_head)))
            continue;

        int png_id = 0;
        void *person = jh_player_person(jh);
        const uintptr_t cfg = reinterpret_cast<uintptr_t>(jh_person_config_ptr(person));
        if (cfg > 0x10000)
            png_id = static_cast<int>(*reinterpret_cast<uint32_t *>(cfg + 120));
        if (png_id <= 0)
            continue;

        void **bh_slot720 = reinterpret_cast<void **>(static_cast<char *>(battle_head) + 720);
        void *bh_bound = bh_slot720 ? *bh_slot720 : nullptr;
        if (bh_bound && looks_like(reinterpret_cast<uintptr_t>(bh_bound)) &&
                jh_player_hp_s32(bh_bound) <= 0)
            continue;

        const uint8_t flag712 = reinterpret_cast<const unsigned char *>(battle_head)[712];

        refresh_battle_head_portrait_from_png_id(battle_head, png_id, true, false);

        if (flag712 != 0)
            to_remove_from_hold.push_back(jh);
    }

    if (!to_remove_from_hold.empty()) {
        pthread_mutex_lock(&g_derivative_schedule_mu);
        for (void *p : to_remove_from_hold)
            g_derivative_shouji_timeline_hold_players.erase(p);
        pthread_mutex_unlock(&g_derivative_schedule_mu);
    }
}

/// libnative `SweepHideDeadPlayersBattleHeadPortraits`（简化：死亡 fade / 存活 cancel）。
void sweep_hide_dead_players_battle_head_portraits_body() {
    void *mgr = counter_hooks_last_battle_mgr_for_portrait_ptr();
    if (!mgr || !looks_like(reinterpret_cast<uintptr_t>(mgr)) || !GameHooks::fn_select_all_players ||
            !GameHooks::fn_get_battle_head)
        return;

    std::vector<void *> vec;
    GameHooks::fn_select_all_players(mgr, false, reinterpret_cast<void *>(&vec));
    GameHooks::fn_select_all_players(mgr, true, reinterpret_cast<void *>(&vec));

    for (void *p : vec) {
        if (!p || !looks_like(reinterpret_cast<uintptr_t>(p)))
            continue;
        void *bh = GameHooks::fn_get_battle_head(p);
        if (!bh || !looks_like(reinterpret_cast<uintptr_t>(bh)))
            continue;
        if (jh_player_hp_s32(p) < 1)
            begin_battle_head_portrait_death_fade_cleanup(bh);
        else
            cancel_battle_head_portrait_death_cleanup(bh);
    }
}

void flush_deferred_derivative_portrait_if_needed_body() {
    const int lw = g_derivative_flush_draw_scene_warmup.load(std::memory_order_relaxed);
    if (lw >= 1) {
        const int prev = g_derivative_flush_draw_scene_warmup.fetch_sub(1, std::memory_order_relaxed);
        if (prev > 1)
            return;
    }

    refresh_derivative_portrait_during_shouji_timeline_if_needed_body();
    sweep_hide_dead_players_battle_head_portraits_body();

    void *saved_pending = nullptr;
    bool skip_pending_refresh = true;
    pthread_mutex_lock(&g_derivative_schedule_mu);
    saved_pending = g_pending_derivative_portrait_player;
    if (saved_pending != nullptr) {
        const bool branch_take_refresh =
                (g_pending_derivative_skips_remaining < 1) ||
                ([&]() {
                    --g_pending_derivative_skips_remaining;
                    return g_pending_derivative_skips_remaining < 1;
                }());
        if (branch_take_refresh) {
            g_pending_derivative_portrait_player = nullptr;
            g_pending_derivative_skips_remaining = 0;
            skip_pending_refresh = false;
        } else {
            GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] flush defer skipsRem=%d",
                    g_pending_derivative_skips_remaining);
            skip_pending_refresh = true;
        }
    } else {
        skip_pending_refresh = true;
    }
    pthread_mutex_unlock(&g_derivative_schedule_mu);

    if (skip_pending_refresh)
        return;

    if (!saved_pending || !looks_like(reinterpret_cast<uintptr_t>(saved_pending)))
        return;
    if (!is_derivative_transformed_battle_player(saved_pending))
        return;
    if (jh_player_hp_s32(saved_pending) <= 0)
        return;

    int png_id = 0;
    void *person = jh_player_person(saved_pending);
    const uintptr_t cfg = reinterpret_cast<uintptr_t>(jh_person_config_ptr(person));
    if (cfg > 0x10000)
        png_id = static_cast<int>(*reinterpret_cast<uint32_t *>(cfg + 120));

    if (png_id <= 0) {
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] flush skip cfg pngId pid=%d",
                jh_player_person_id(saved_pending));
        return;
    }

    void *bh = GameHooks::fn_get_battle_head ? GameHooks::fn_get_battle_head(saved_pending) : nullptr;
    if (!bh || !looks_like(reinterpret_cast<uintptr_t>(bh))) {
        GM_LOG(ANDROID_LOG_ERROR, "[DerivativePortrait] flush skip battleHead pid=%d",
                jh_player_person_id(saved_pending));
        return;
    }

    // 对齐 libnative FlushDeferred：只 RefreshBattleHeadPortraitFromPlistPngId；
    // 勿在此处 fuHuo/updateHead（会强制刷 Buff HUD → 出场全队 Buff 闪一下就还原）。
    refresh_battle_head_portrait_from_png_id(bh, png_id, true, true);
}

void derivative_notify_battle_head_been_attack_post_impl(void *battle_head_this) {
    if (!battle_head_this || !looks_like(reinterpret_cast<uintptr_t>(battle_head_this)))
        return;
    void **slot720 = reinterpret_cast<void **>(static_cast<char *>(battle_head_this) + 720);
    void *jh_player = slot720 ? *slot720 : nullptr;
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;
    void *bm = jh_player_battle_mgr(jh_player);
    if (bm)
        counter_hooks_set_last_battle_mgr_for_portrait(bm);
    set_derivative_shouji_portrait_timeline_hold(jh_player);
    if (is_derivative_transformed_battle_player(jh_player))
        schedule_derivative_portrait_refresh_soon(jh_player);
}

void derivative_notify_battle_head_last_shouji_post_impl(void *battle_head_this) {
    if (!battle_head_this || !looks_like(reinterpret_cast<uintptr_t>(battle_head_this)))
        return;
    void **slot720 = reinterpret_cast<void **>(static_cast<char *>(battle_head_this) + 720);
    void *jh_player = slot720 ? *slot720 : nullptr;
    if (!jh_player || !looks_like(reinterpret_cast<uintptr_t>(jh_player)))
        return;
    if (is_derivative_transformed_battle_player(jh_player))
        schedule_derivative_portrait_refresh_soon(jh_player);
}

} // namespace mod_add_hp_i

namespace ModAddHpExtras {

void trace_hunluan_add_hp_if_remapped(void *vic_raw_jh,
        void *vic_eff_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type) {
    if (!vic_raw_jh || !vic_eff_jh || vic_raw_jh == vic_eff_jh || !attacker_jh)
        return;
    const unsigned hp_u = static_cast<unsigned>(hp_amt);
    if ((hp_u & 0x80000000u) == 0u)
        return;
    const int round_ev = CounterState::current_round_for_trace();
    AH_LOG(ANDROID_LOG_ERROR,
            "[TraceHunluanAddHp] round=%d atk=%d vicRaw=%d vicEff=%d hpAmt=%d dmgType=%d atkConfused=%d",
            round_ev,
            jh_player_person_id(attacker_jh),
            jh_player_person_id(vic_raw_jh),
            jh_player_person_id(vic_eff_jh),
            hp_amt,
            dmg_type,
            ModConfusion::is_active_for_player(attacker_jh) ? 1 : 0);
}

void trace_add_hp_confusion_conditional(void *vic_raw_jh,
        void *vic_eff_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type,
        int hp_before_first_damage,
        int hp_after_final) {
    if (!attacker_jh || !vic_raw_jh || vic_raw_jh == attacker_jh)
        return;
    const unsigned hp_u = static_cast<unsigned>(hp_amt);
    if ((hp_u & 0x80000000u) == 0u)
        return;
    if (!ModConfusion::is_active_for_player(vic_eff_jh) && !ModConfusion::is_active_for_player(attacker_jh))
        return;

    void *tls_actor = counter_hooks_tls_skill_actor_ptr();
    const int tls_pid = tls_actor ? jh_player_person_id(tls_actor) : -1;
    const int round_ev = CounterState::current_round_for_trace();
    AH_LOG(ANDROID_LOG_ERROR,
            "[TraceAddHp] round=%d dmgType=%d atk=%d vicRaw=%d vicEff=%d amt=%d hpB=%d hpA=%d vicConf=%d atkConf=%d tlsPid=%d",
            round_ev,
            dmg_type,
            jh_player_person_id(attacker_jh),
            jh_player_person_id(vic_raw_jh),
            jh_player_person_id(vic_eff_jh),
            hp_amt,
            hp_before_first_damage,
            hp_after_final,
            ModConfusion::is_active_for_player(vic_eff_jh) ? 1 : 0,
            ModConfusion::is_active_for_player(attacker_jh) ? 1 : 0,
            tls_pid);
}

void after_native_addhp_chunk(void *victim_effective_jh, int hp_amt) {
    mod_add_hp_i::sub_a4ce4_maybe_refresh_derivative_portrait(victim_effective_jh, hp_amt);
}

void finalize_after_confusion_traces(void *vic_raw_jh,
        void *vic_eff_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type,
        bool add_hp_arg_a5,
        bool add_hp_arg_a6,
        int hp_before_first_damage,
        int hp_after_final) {
    const unsigned hp_u = static_cast<unsigned>(hp_amt);

    mod_add_hp_i::trace_wugong_chaos_3001(attacker_jh,
            vic_eff_jh,
            vic_raw_jh,
            hp_u,
            hp_before_first_damage,
            hp_after_final,
            dmg_type,
            add_hp_arg_a5,
            add_hp_arg_a6);

    const uint32_t nei = jh_player_person_neigong_id_u32(vic_eff_jh);
    const uint32_t pers = jh_player_personality_id(vic_eff_jh);
    const bool gate = (pers == 51u) || (nei == 3897u);
    if (gate) {
        mod_add_hp_i::notify_battle_changed_for_hurt_dr(vic_eff_jh);
        mod_add_hp_i::apply_tian_xing_51(vic_eff_jh);
    }

    void *bh_for_derivative = nullptr;
    if (jh_player_hp_s32(vic_eff_jh) <= 0 && jh_player_personality_id(vic_eff_jh) == 100u &&
        GameHooks::fn_get_battle_head)
        bh_for_derivative = GameHooks::fn_get_battle_head(vic_eff_jh);

    if (jh_player_hp_s32(vic_eff_jh) <= 0 && jh_player_personality_id(vic_eff_jh) == 100u)
        mod_add_hp_i::apply_derivative_on_death(vic_eff_jh, bh_for_derivative);

    if (GameHooks::fn_get_battle_head && jh_player_hp_s32(vic_eff_jh) <= 0) {
        void *bh = GameHooks::fn_get_battle_head(vic_eff_jh);
        mod_add_hp_i::begin_battle_head_portrait_death_fade_cleanup(bh);
    }
}

void flush_deferred_derivative_portrait_if_needed() {
    mod_add_hp_i::flush_deferred_derivative_portrait_if_needed_body();
}

void derivative_notify_battle_head_been_attack_post(void *battle_head_this) {
    mod_add_hp_i::derivative_notify_battle_head_been_attack_post_impl(battle_head_this);
}

void derivative_notify_battle_head_last_shouji_post(void *battle_head_this) {
    mod_add_hp_i::derivative_notify_battle_head_last_shouji_post_impl(battle_head_this);
}

void on_battle_end() {
    mod_add_hp_i::derivative_act_flags_on_new_fight_round_body();
    mod_add_hp_i::zhusha_amp_clear_all_on_battle_end_body();

    pthread_mutex_lock(&mod_add_hp_i::g_derivative_schedule_mu);
    mod_add_hp_i::g_derivative_shouji_timeline_hold_players.clear();
    mod_add_hp_i::g_pending_derivative_portrait_player = nullptr;
    mod_add_hp_i::g_pending_derivative_skips_remaining = 0;
    pthread_mutex_unlock(&mod_add_hp_i::g_derivative_schedule_mu);

    std::vector<std::pair<void *, mod_add_hp_i::DerivativePreSnap>> deriv_restore_batch;
    pthread_mutex_lock(&mod_add_hp_i::g_derivative_restore_mu);
    deriv_restore_batch.reserve(mod_add_hp_i::g_deriv_restore_by_jh.size());
    for (auto &kv : mod_add_hp_i::g_deriv_restore_by_jh)
        deriv_restore_batch.emplace_back(reinterpret_cast<void *>(kv.first), kv.second);
    mod_add_hp_i::g_deriv_restore_by_jh.clear();
    pthread_mutex_unlock(&mod_add_hp_i::g_derivative_restore_mu);

    for (auto &pr : deriv_restore_batch)
        mod_add_hp_i::restore_derivative_person_from_snap_after_battle(pr.first, pr.second);

    pthread_mutex_lock(&mod_add_hp_i::g_hurt_dr_mu);
    mod_add_hp_i::g_pid_to_battle_mgr_for_hurt_dr.clear();
    pthread_mutex_unlock(&mod_add_hp_i::g_hurt_dr_mu);

    pthread_mutex_lock(&mod_add_hp_i::g_bh_death_mu);
    mod_add_hp_i::g_bh_death_cleanup_armed.clear();
    pthread_mutex_unlock(&mod_add_hp_i::g_bh_death_mu);

    mod_add_hp_i::g_derivative_flush_draw_scene_warmup.store(2, std::memory_order_relaxed);
}

void derivative_act_flags_on_new_fight_round() {
    mod_add_hp_i::derivative_act_flags_on_new_fight_round_body();
}

void zhusha_amp_on_new_fight_round(int fight_round_param) {
    mod_add_hp_i::zhusha_amp_on_new_fight_round_body(fight_round_param);
}

void maybe_derivative_qingying_counter_on_damage(void *attacker_jh,
        void *victim_effective_jh,
        unsigned hp_amt_unsigned) {
    mod_add_hp_i::maybe_derivative_qingying_counter_on_damage_body(attacker_jh, victim_effective_jh, hp_amt_unsigned);
}

void maybe_derivative_jinhuang_extra_turn_after_begin_act(void *jh_skill_owner) {
    mod_add_hp_i::maybe_derivative_jinhuang_extra_turn_after_begin_act_body(jh_skill_owner);
}

} // namespace ModAddHpExtras
//傻逼蛇咬，尽孝你m呢