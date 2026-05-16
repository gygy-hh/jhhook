#include "mod_confusion.hpp"

#include <android/log.h>
#include <pthread.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include "counter_hooks.hpp"
#include "dobby.h"
#include "counter_state.hpp"
#include "game_hooks.hpp"
#include "jh_abi.hpp"
#include "mod_add_hp_extras.hpp"

#define MC_LOG(prio, ...) __android_log_print(prio, "ModConfusion", __VA_ARGS__)
#define CAM_LOG(prio, ...) __android_log_print(prio, "CounterAttackMod", __VA_ARGS__)

/// 同一混乱出手方在本招式内共用一个替身指针（多次 `getActShouJiPlayer`/`addHp` 禁止重复 rand）。
static thread_local void *g_conf_subst_tls_attacker{};
static thread_local void *g_conf_subst_tls_pick{};

static void confusion_subst_tls_clear() {
    g_conf_subst_tls_attacker = nullptr;
    g_conf_subst_tls_pick = nullptr;
}

namespace {

std::atomic<int> g_fight_round{0};

bool state_handle_new_fight_round(int round_param) {
    if (round_param < 0)
        return false;
    int prev = g_fight_round.load(std::memory_order_relaxed);
    if (prev == round_param)
        return false;
    g_fight_round.store(round_param, std::memory_order_relaxed);
    return true;
}

int baseline_round_confusion() {
    int r = g_fight_round.load(std::memory_order_relaxed);
    return r <= 0 ? 1 : r;
}

pthread_mutex_t g_conf_mutex = PTHREAD_MUTEX_INITIALIZER;
std::map<uintptr_t, int> g_expire_round_by_actor;

/// `onNewFightRound` 会对场上每名 JhPlayer 各调用一次；仅首张与其它「有信息量」回调打印 TraceConfusion。
int g_trace_confusion_last_logged_round = -999999;

struct ConfusionFacingPack {
    void *attacker{};
    void *head{};
    float rotation_backup{};
    float scale_backup{};
    uint8_t use_scale_flip{};
    uint8_t active{};
};

thread_local ConfusionFacingPack g_facing_tls{};

void confusion_facing_restore(const char *reason);

/// 按帧延后还原肖像翻面（与 confused_allied_damage 跳过 addHp-post 配合）。
std::atomic<int> g_confusion_facing_extend_frames_left{0};
static constexpr int kConfusionFacingExtendDrawFrames = 96;

static void confusion_facing_extend_schedule() {
    g_confusion_facing_extend_frames_left.store(
            kConfusionFacingExtendDrawFrames, std::memory_order_relaxed);
}

static void confusion_facing_extend_cancel() {
    g_confusion_facing_extend_frames_left.store(0, std::memory_order_relaxed);
}

static void confusion_facing_extend_tick_after_draw() {
    int v = g_confusion_facing_extend_frames_left.load(std::memory_order_relaxed);
    if (v <= 0)
        return;
    v -= 1;
    g_confusion_facing_extend_frames_left.store(v, std::memory_order_relaxed);
    if (v != 0)
        return;
    confusion_facing_restore("facingExtendDrawFrames");
}

void confusion_facing_restore(const char *reason) {
    if (!g_facing_tls.active)
        return;
    confusion_facing_extend_cancel();
    void *h = g_facing_tls.head;
    if (h) {
        if (g_facing_tls.use_scale_flip && GameHooks::fn_set_scale_x) {
            GameHooks::fn_set_scale_x(h, g_facing_tls.scale_backup);
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[ConfusionFacing][%s] restore-active scaleX head=%p old=%f attacker=%p",
                    reason,
                    h,
                    g_facing_tls.scale_backup,
                    g_facing_tls.attacker);
        } else if (!g_facing_tls.use_scale_flip && GameHooks::fn_set_rotation_y) {
            GameHooks::fn_set_rotation_y(h, g_facing_tls.rotation_backup);
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[ConfusionFacing][%s] restore-active rotationY head=%p old=%f attacker=%p",
                    reason,
                    h,
                    g_facing_tls.rotation_backup,
                    g_facing_tls.attacker);
        }
    }
    std::memset(&g_facing_tls, 0, sizeof(g_facing_tls));
}

void confusion_facing_apply(void *attacker, void *target, const char *tag) {
    if (!attacker || !target || attacker == target)
        return;
    if (!ModConfusion::is_active_for_player(attacker))
        return;
    if (!jh_player_same_side(attacker, target))
        return;
    if (!GameHooks::fn_get_battle_head)
        return;

    if (g_facing_tls.active) {
        if (g_facing_tls.attacker == attacker)
            return;
        confusion_facing_restore("switch-attacker");
    }

    void *battle_head = GameHooks::fn_get_battle_head(attacker);
    if (!battle_head) {
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[ConfusionFacing][%s] skip: head null attacker=%p target=%p",
                tag,
                attacker,
                target);
        return;
    }

    if (GameHooks::fn_get_scale_x && GameHooks::fn_set_scale_x) {
        float old_scale = GameHooks::fn_get_scale_x(battle_head);
        float neu = -old_scale;
        if (neu == 0.f)
            neu = -1.f;
        GameHooks::fn_set_scale_x(battle_head, neu);
        g_facing_tls.attacker = attacker;
        g_facing_tls.head = battle_head;
        g_facing_tls.rotation_backup = 0.f;
        g_facing_tls.scale_backup = old_scale;
        g_facing_tls.use_scale_flip = 1;
        g_facing_tls.active = 1;
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[ConfusionFacing][%s] apply-active scaleX head=%p old=%f new=%f attacker=%d target=%d",
                tag,
                battle_head,
                old_scale,
                neu,
                jh_player_person_id(attacker),
                jh_player_person_id(target));
        return;
    }

    if (GameHooks::fn_get_rotation_y && GameHooks::fn_set_rotation_y) {
        float old_rot = GameHooks::fn_get_rotation_y(battle_head);
        GameHooks::fn_set_rotation_y(battle_head, old_rot + 180.f);
        g_facing_tls.attacker = attacker;
        g_facing_tls.head = battle_head;
        g_facing_tls.rotation_backup = old_rot;
        g_facing_tls.scale_backup = 1.f;
        g_facing_tls.use_scale_flip = 0;
        g_facing_tls.active = 1;
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[ConfusionFacing][%s] apply-active rotationY head=%p old=%f new=%f attacker=%d target=%d",
                tag,
                battle_head,
                old_rot,
                old_rot + 180.f,
                jh_player_person_id(attacker),
                jh_player_person_id(target));
        return;
    }

    CAM_LOG(ANDROID_LOG_ERROR, "[ConfusionFacing][%s] skip: no API", tag);
}

void *pick_confusion_substitute(void *attacker, void *original_target, void *battle_mgr) {
    if (!attacker || !battle_mgr || !GameHooks::fn_select_all_players)
        return original_target;

    const bool side_nonzero = jh_player_side_byte(attacker) != 0;
    std::vector<void *> bucket;
    GameHooks::fn_select_all_players(battle_mgr, side_nonzero, reinterpret_cast<void *>(&bucket));

    std::vector<void *> allies;
    allies.reserve(bucket.size());
    for (void *p : bucket) {
        if (!p || p == attacker)
            continue;
        if (static_cast<int>(jh_player_hp_u32(p)) <= 0)
            continue;
        if (!jh_player_same_side(attacker, p))
            continue;
        allies.push_back(p);
    }

    if (allies.empty()) {
        if (original_target && original_target != attacker &&
            jh_player_same_side(attacker, original_target) &&
            static_cast<int>(jh_player_hp_u32(original_target)) >= 1)
            return original_target;
        return original_target;
    }

    // 引擎已指定己方活人：以引擎为准，并丢弃本轮缓存的 rand 替身（避免与后续帧混淆）。
    if (original_target && original_target != attacker &&
        jh_player_same_side(attacker, original_target) &&
        static_cast<int>(jh_player_hp_u32(original_target)) >= 1) {
        if (g_conf_subst_tls_attacker == attacker)
            g_conf_subst_tls_pick = nullptr;
        return original_target;
    }

    if (g_conf_subst_tls_attacker == attacker && g_conf_subst_tls_pick) {
        void *c = g_conf_subst_tls_pick;
        if (c && c != attacker && jh_player_same_side(attacker, c) &&
            static_cast<int>(jh_player_hp_u32(c)) >= 1) {
            for (void *p : allies) {
                if (p == c)
                    return c;
            }
        }
    }

    const int n = static_cast<int>(allies.size());
    const int idx = rand() % n;
    void *chosen = allies[static_cast<size_t>(idx)];
    g_conf_subst_tls_attacker = attacker;
    g_conf_subst_tls_pick = chosen;
    return chosen;
}

static int64_t hooked_on_new_fight_round(void *jh_this, int round_param) {
    void *battle_mgr = jh_player_battle_mgr(jh_this);

    CounterState::handle_new_fight_round(static_cast<int>(round_param));
    ModAddHpExtras::derivative_act_flags_on_new_fight_round();

    const bool queue_changed = state_handle_new_fight_round(static_cast<int>(round_param));
    ModConfusion::on_new_fight_round(static_cast<int>(round_param));

    const int cb_pid = jh_this ? jh_player_person_id(jh_this) : -1;
    const int state_round = g_fight_round.load(std::memory_order_relaxed);

    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[TraceRound] onNewFightRound cbPid=%d paramRound=%d stateRoundNow=%d roundQueueChanged=%d battleMgr=%p",
            cb_pid,
            round_param,
            state_round,
            queue_changed ? 1 : 0,
            battle_mgr);

    static void *s_last_mgr_for_trace = nullptr;
    if (battle_mgr && GameHooks::fn_select_all_players && battle_mgr != s_last_mgr_for_trace) {
        s_last_mgr_for_trace = battle_mgr;
        std::vector<void *> vec;
        GameHooks::fn_select_all_players(battle_mgr, false, reinterpret_cast<void *>(&vec));

        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[TraceTianxing99] battleFirstTick mgr=%p sideFlag=%d allySlotCount=%zu",
                battle_mgr,
                0,
                vec.size());

        for (void *slot : vec) {
            if (!slot)
                continue;
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[TraceTianxing99] scan pid=%d pers=%u hp=%u skill=? neigong=?",
                    jh_player_person_id(slot),
                    jh_player_personality_id(slot),
                    jh_player_hp_u32(slot));

            constexpr uint32_t kPersonalityHunluan = 99;
            if (jh_player_personality_id(slot) == kPersonalityHunluan &&
                static_cast<int>(jh_player_hp_u32(slot)) >= 1) {
                ModConfusion::apply(slot, 7);
                CAM_LOG(
                        ANDROID_LOG_ERROR,
                        "[TraceTianxing99] apply confusion pid=%d addBuffRounds=%d",
                        jh_player_person_id(slot),
                        7);
            }
        }
    }

    if (!GameHooks::orig_on_new_fight_round)
        return 0;
    return GameHooks::orig_on_new_fight_round(jh_this, round_param);
}

static void *hooked_get_act_shouji_player(void *jh_this) {
    if (!GameHooks::orig_get_act_shouji_player)
        return nullptr;

    void *original = GameHooks::orig_get_act_shouji_player(jh_this);
    if (!jh_this)
        return original;

    if (!ModConfusion::is_active_for_player(jh_this))
        return original;

    // 仅当受害 JhPlayer（身上有混乱）被调用且 TLS 记录的是非混乱的真正攻击者时跳过替换，
    // 避免「我方打混乱敌人」时误把 pick 当成敌人的出手替身。
    // 肖像翻面：对齐原生 sub_A2534，在 remap 后的 getActShouJiPlayer 路径调用 confusion_facing_apply（SkillOneTime 入口目标列表通常尚未就绪）。
    void *tls_actor = counter_hooks_tls_skill_actor_ptr();
    if (tls_actor && tls_actor != jh_this && ModConfusion::is_active_for_player(jh_this) &&
        !ModConfusion::is_active_for_player(tls_actor)) {
        MC_LOG(ANDROID_LOG_ERROR,
                "[HUNLUAN] getActShouJiPlayer skipRemap tlsPid=%d jhPid=%d (victim confusion_ctx)",
                jh_player_person_id(tls_actor),
                jh_player_person_id(jh_this));
        return original;
    }

    void *mgr = jh_player_battle_mgr(jh_this);
    void *picked = pick_confusion_substitute(jh_this, original, mgr);
    // 对齐原生 sub_A2534：逻辑受击为己方活人时翻面（发生在 beginAct 遍历目标时，比 SkillOneTime 入口 scratch 更可靠）。
    if (picked && picked != jh_this && jh_player_same_side(jh_this, picked) &&
        static_cast<int>(jh_player_hp_u32(picked)) >= 1) {
        if (picked != original)
            CAM_LOG(ANDROID_LOG_ERROR, "[HUNLUAN] getActShouJiPlayer remap %d -> %d",
                    jh_player_person_id(original),
                    jh_player_person_id(picked));
        confusion_facing_apply(jh_this, picked, "getActShouJiPlayer");
    }
    return picked;
}

} // namespace

namespace ModConfusion {

void install_add_buffer_hook_if_needed() {
    // JhPlayer::addBuffer 已由 CounterAttackMod（counter_hooks）挂载；此处保留占位供初始化兼容。
}

void on_new_fight_round(int round_param) {
    if (round_param < 0)
        return;

    ModAddHpExtras::zhusha_amp_on_new_fight_round(round_param);

    pthread_mutex_lock(&g_conf_mutex);
    const size_t before = g_expire_round_by_actor.size();
    size_t cleared = 0;
    for (auto it = g_expire_round_by_actor.begin(); it != g_expire_round_by_actor.end(); ) {
        if (round_param >= it->second) {
            it = g_expire_round_by_actor.erase(it);
            ++cleared;
        } else {
            ++it;
        }
    }
    const size_t after = g_expire_round_by_actor.size();

    const bool dup_same_round = (round_param == g_trace_confusion_last_logged_round);
    if (!dup_same_round)
        g_trace_confusion_last_logged_round = round_param;
    const bool interesting = (cleared != 0u || before != 0u || after != 0u);

    if (!dup_same_round || interesting) {
        MC_LOG(
                ANDROID_LOG_ERROR,
                "[TraceConfusion] onNewFightRound param=%d cleared=%zu mapBefore=%zu mapAfter=%zu",
                round_param,
                cleared,
                before,
                after);

        for (const auto &kv : g_expire_round_by_actor) {
            const bool still_active = round_param < kv.second;
            MC_LOG(
                    ANDROID_LOG_ERROR,
                    "[TraceConfusion]   active actor=%p expiresBeforeRound=%d (curParam=%d stillActive=%d)",
                    reinterpret_cast<const void *>(kv.first),
                    kv.second,
                    round_param,
                    still_active ? 1 : 0);
        }
    }

    pthread_mutex_unlock(&g_conf_mutex);
}

void apply(void *actor, intptr_t duration_rounds_disguised) {
    if (!actor || duration_rounds_disguised <= 0)
        return;

    const int add = static_cast<int>(duration_rounds_disguised);
    const int base = baseline_round_confusion();
    const int expire = base + add;
    const uintptr_t key = reinterpret_cast<uintptr_t>(actor);

    pthread_mutex_lock(&g_conf_mutex);
    auto it = g_expire_round_by_actor.find(key);
    bool should_write = true;
    if (it != g_expire_round_by_actor.end())
        should_write = expire > it->second;

    if (should_write) {
        g_expire_round_by_actor[key] = expire;
        MC_LOG(
                ANDROID_LOG_ERROR,
                "[TraceConfusion] apply set ptr=%p pid=%d pers=%u expire=%d base=%d add=%d",
                actor,
                jh_player_person_id(actor),
                jh_player_personality_id(actor),
                expire,
                base,
                add);
    } else {
        MC_LOG(
                ANDROID_LOG_ERROR,
                "[TraceConfusion] apply skip ptr=%p pid=%d curExpire=%d want=%d base=%d add=%d",
                actor,
                jh_player_person_id(actor),
                it->second,
                expire,
                base,
                add);
    }
    pthread_mutex_unlock(&g_conf_mutex);
}

void apply_overwrite(void *actor, intptr_t duration_rounds_disguised) {
    if (!actor || duration_rounds_disguised <= 0)
        return;
    const int add = static_cast<int>(duration_rounds_disguised);
    const int base = baseline_round_confusion();
    const int expire = base + add;
    const uintptr_t key = reinterpret_cast<uintptr_t>(actor);

    pthread_mutex_lock(&g_conf_mutex);
    g_expire_round_by_actor[key] = expire;
    MC_LOG(
            ANDROID_LOG_ERROR,
            "[TraceConfusion] applyOverwrite ptr=%p pid=%d pers=%u expire=%d base=%d add=%d",
            actor,
            jh_player_person_id(actor),
            jh_player_personality_id(actor),
            expire,
            base,
            add);
    pthread_mutex_unlock(&g_conf_mutex);
}

void clear_actor(void *actor) {
    if (!actor)
        return;
    pthread_mutex_lock(&g_conf_mutex);
    const uintptr_t key = reinterpret_cast<uintptr_t>(actor);
    const auto erased = g_expire_round_by_actor.erase(key);
    if (erased)
        MC_LOG(
                ANDROID_LOG_ERROR,
                "[TraceConfusion] clearActor ptr=%p pid=%d",
                actor,
                jh_player_person_id(actor));
    pthread_mutex_unlock(&g_conf_mutex);
}

void clear_all() {
    confusion_subst_tls_clear();
    pthread_mutex_lock(&g_conf_mutex);
    g_trace_confusion_last_logged_round = -999999;
    const size_t n = g_expire_round_by_actor.size();
    g_expire_round_by_actor.clear();
    if (n)
        MC_LOG(ANDROID_LOG_ERROR, "[TraceConfusion] clearAll removed=%zu entries", n);
    pthread_mutex_unlock(&g_conf_mutex);
}

bool is_active_for_player(void *jh_player) {
    if (!jh_player)
        return false;
    const uint32_t hp = jh_player_hp_u32(jh_player);
    if (hp & 0x80000000u)
        return false;

    const int cur = baseline_round_confusion();
    const uintptr_t key = reinterpret_cast<uintptr_t>(jh_player);

    pthread_mutex_lock(&g_conf_mutex);
    auto it = g_expire_round_by_actor.find(key);
    const bool ok = it != g_expire_round_by_actor.end() && cur < it->second;
    pthread_mutex_unlock(&g_conf_mutex);
    return ok;
}

int fight_round_snapshot() {
    return g_fight_round.load(std::memory_order_relaxed);
}

void confusion_facing_restore_external(const char *reason_tag) {
    confusion_facing_restore(reason_tag);
}

void confusion_facing_schedule_extend_after_confused_allied_damage() {
    confusion_facing_extend_schedule();
}

void confusion_facing_tick_extend_after_draw_scene() {
    confusion_facing_extend_tick_after_draw();
}

void confusion_facing_apply_external(void *attacker, void *target, const char *tag) {
    confusion_facing_apply(attacker, target, tag);
}

void *pick_confusion_substitute_external(void *attacker, void *preferred_target_or_null, void *battle_mgr) {
    return pick_confusion_substitute(attacker, preferred_target_or_null, battle_mgr);
}

void clear_confusion_shouji_substitute_tls_cache() {
    confusion_subst_tls_clear();
}

void confusion_facing_before_addhp_cleanup(void *attacker_jh_player, int hp_amt) {
    if (!g_facing_tls.active)
        return;
    const unsigned hp_u = static_cast<unsigned>(hp_amt);
    const bool skip = attacker_jh_player != nullptr && (hp_u & 0x80000000u) != 0u &&
                      g_facing_tls.attacker == attacker_jh_player;
    if (!skip)
        confusion_facing_restore("addHp-pre-cleanup");
}

void confusion_facing_after_addhp_restore(void *attacker_jh_player) {
    if (!attacker_jh_player || !g_facing_tls.active)
        return;
    if (g_facing_tls.attacker == attacker_jh_player)
        confusion_facing_restore("addHp-post");
}

void install_confusion_hooks(uintptr_t cocos_elf_bias) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    GameHooks::resolve(cocos_elf_bias);
    install_add_buffer_hook_if_needed();

    void *p_round =
            reinterpret_cast<void *>(cocos_elf_bias + 0x5CFED4u); // JhPlayer::onNewFightRound(int)
    void *p_shouji =
            reinterpret_cast<void *>(cocos_elf_bias + 0x5CBA04u); // JhPlayer::getActShouJiPlayer(void)

    int rc = DobbyHook(
            p_round,
            reinterpret_cast<dobby_dummy_func_t>(hooked_on_new_fight_round),
            reinterpret_cast<dobby_dummy_func_t *>(&GameHooks::orig_on_new_fight_round));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] onNewFightRound rc=%d hook=%p orig=%p",
            rc,
            p_round,
            reinterpret_cast<void *>(GameHooks::orig_on_new_fight_round));

    rc = DobbyHook(
            p_shouji,
            reinterpret_cast<dobby_dummy_func_t>(hooked_get_act_shouji_player),
            reinterpret_cast<dobby_dummy_func_t *>(&GameHooks::orig_get_act_shouji_player));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] getActShouJiPlayer rc=%d hook=%p orig=%p",
            rc,
            p_shouji,
            reinterpret_cast<void *>(GameHooks::orig_get_act_shouji_player));
}

} // namespace ModConfusion
//蛇咬爹