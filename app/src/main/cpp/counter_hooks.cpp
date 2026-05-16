#include "counter_hooks.hpp"

#include <android/log.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "dobby.h"
#include "counter_state.hpp"
#include "game_hooks.hpp"
#include "jh_abi.hpp"
#include "mod_add_hp_extras.hpp"
#include "mod_confusion.hpp"

thread_local void *g_tls_skill_actor = nullptr;

/// libnative `GaoyangMod::CounterAttackMod::g_lastBattleMgrForPortrait`
static void *g_last_battle_mgr_for_portrait = nullptr;

#define CAM_LOG(prio, ...) __android_log_print(prio, "CounterAttackMod", __VA_ARGS__)

namespace {

constexpr uintptr_t kAddHp = 0x5CEED8u; // _ZN8JhPlayer5addHpEPS_iibbN7cocos2d4Vec2E
constexpr uintptr_t kOnRoundBegin = 0x5CB258u; // _ZN8JhPlayer12onRoundBeginEv
constexpr uintptr_t kSkillOneTime_onAttackEvent = 0x665958u; // _ZN12SkillOneTime14onAttatckEventERKSs
constexpr uintptr_t kSkillOneTime_beginAct = 0x665F88u; // _ZN12SkillOneTime8beginActEv

constexpr uintptr_t kJhPlayer_addBuffer_Buff = 0x5CAF94u;   // JhPlayer::addBuffer(int,Buff*)
constexpr uintptr_t kJhPlayer_addBuffer_BuffTr = 0x5CB074u; // JhPlayer::addBuffer(int,BuffTr*,int,int)

// lst：BattleHead / Battle（对齐 libnative InitializeHooks + sub_A6214/A6310/A63D4 挂载点）
constexpr uintptr_t kBattleHead_updateBuff = 0x4F7DC8u;              // BattleHead::updateBuff(void)
constexpr uintptr_t kBattleHead_beenAttack = 0x4F6C58u;               // BattleHead::beenAttack(bool)
constexpr uintptr_t kBattleHead_onLastShouJiFrameImpl = 0x4F5808u;   // BattleHead::onLastShouJiFrameImpl(float)
constexpr uintptr_t kBattle_onEndBattle = 0x4EDAA4u;                 // Battle::onEndBattle(float)
constexpr uintptr_t kDirector_drawScene = 0x8E71E8u;                  // cocos2d::Director::drawScene(void)

using JhPlayer_addHp_t = void (*)(void *this_victim, void *other_attacker, int hp_amt, int dmg_type, bool a5,
                                  bool a6, void *vec2_indirect);

using JhPlayer_onRoundBegin_t = int64_t (*)(void *jh_this);

/// lst: `SkillOneTime::onAttatckEvent(std::string const&)` — 第二参数为引用，调用约定中等价于 `const std::string*`。
using SkillOneTime_onAttackEvent_t = int64_t (*)(void *skill_this, const void *attack_event_string_ref);

using SkillOneTime_beginAct_t = int64_t (*)(void *skill_this);

using JhPlayer_addBuffer_Buff_t = int64_t (*)(void *jh_this, int buff_id, void *buff_ptr);

using JhPlayer_addBuffer_BuffTr_t = int64_t (*)(void *jh_this, int buff_id, void *buff_tr, int a4, int a5);

using BattleHead_updateBuff_t = int64_t (*)(void *battle_head_this);

using BattleHead_beenAttack_t = int64_t (*)(void *battle_head_this, bool flag);

using BattleHead_onLastShouJiFrameImpl_t = int64_t (*)(void *battle_head_this, float dt);

using Battle_onEndBattle_t = int64_t (*)(void *battle_this, float dt);

using Director_drawScene_t = void (*)(void *director_this);

JhPlayer_addHp_t g_orig_add_hp = nullptr;

JhPlayer_onRoundBegin_t g_orig_on_round_begin = nullptr;

SkillOneTime_onAttackEvent_t g_orig_skill_on_attack_event = nullptr;

SkillOneTime_beginAct_t g_orig_skill_begin_act = nullptr;

JhPlayer_addBuffer_Buff_t g_orig_add_buffer_buff = nullptr;

JhPlayer_addBuffer_BuffTr_t g_orig_add_buffer_buff_tr = nullptr;

BattleHead_updateBuff_t g_orig_battle_head_update_buff = nullptr;

BattleHead_beenAttack_t g_orig_battle_head_been_attack = nullptr;

BattleHead_onLastShouJiFrameImpl_t g_orig_battle_head_last_shouji_impl = nullptr;

Battle_onEndBattle_t g_orig_battle_on_end_battle = nullptr;

Director_drawScene_t g_orig_director_draw_scene = nullptr;

struct SkillFacingScratch {
    void *battle_head{};
    float rotation_backup{};
    float scale_backup{};
    uint8_t mirror_active{};
    uint8_t use_scale_flip{};
};

static void scratch_clear_skill_mirror(SkillFacingScratch *s) {
    std::memset(s, 0, sizeof(*s));
    s->scale_backup = 1.f;
}

static void scratch_restore_skill_mirror(SkillFacingScratch *s) {
    if (!(s->mirror_active & 1u) || !s->battle_head)
        return;
    if ((s->use_scale_flip & 1u) != 0 && GameHooks::fn_set_scale_x)
        GameHooks::fn_set_scale_x(s->battle_head, s->scale_backup);
    else if (GameHooks::fn_set_rotation_y)
        GameHooks::fn_set_rotation_y(s->battle_head, s->rotation_backup);
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[ConfusionFacing][skillScratchRestore] head=%p useScale=%u",
            s->battle_head,
            static_cast<unsigned>(s->use_scale_flip));
    scratch_clear_skill_mirror(s);
}

static void *skill_owner_player(void *skill_this) {
    if (!skill_this)
        return nullptr;
    return *reinterpret_cast<void **>(static_cast<char *>(skill_this) + 16);
}

/// 原生 sub_A2BF4 挂在 SkillOneTime 入口、早于 `SkillBase::beginAct`，目标 vector 常仍为空，镜面应在 getActShouJiPlayer（sub_A2534）与 addHp 链路由 confusion_facing_apply 完成；此处保持空实现以免双重翻面。
static void scratch_prepare_skill_mirror(void *skill_this, SkillFacingScratch *s) {
    scratch_clear_skill_mirror(s);
    (void) skill_this;
}

/// libnative `JhPlayer_addHp_hook` 末尾还击：内功 3828/3864/4000 + fightInsert / skillInit / changeSkill。
static void add_hp_maybe_counter_retaliate(void *attacker, void *vic_eff, unsigned hp_u) {
    if (!attacker || vic_eff == attacker || (hp_u & 0x80000000u) == 0u)
        return;

    const bool confused_hit_ally =
            ModConfusion::is_active_for_player(attacker) && jh_player_same_side(attacker, vic_eff);
    if (confused_hit_ally)
        return;

    const uint32_t nid = jh_player_person_neigong_id_u32(vic_eff);
    const bool neigong_gate = nid == 3828u || nid == 3864u || nid == 4000u;
    if (!neigong_gate)
        return;

    if (!CounterState::is_first_damage_this_round(attacker, vic_eff))
        return;

    int threshold = 0;
    if (nid == 3828u)
        threshold = 35;
    else if (nid == 3864u)
        threshold = 20;
    else
        threshold = 100;

    const int roll = (std::rand() % 100) + 1;
    if (roll > threshold) {
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[CounterRetaliate] rollFail vicPid=%d neigong=%u roll=%d thr=%d",
                jh_player_person_id(vic_eff),
                nid,
                roll,
                threshold);
        return;
    }

    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[CounterRetaliate] trigger vicPid=%d neigong=%u roll=%d thr=%d atkPid=%d",
            jh_player_person_id(vic_eff),
            nid,
            roll,
            threshold,
            jh_player_person_id(attacker));

    if (nid == 3864u) {
        CAM_LOG(ANDROID_LOG_ERROR, "[CounterRetaliate] neigong 3864 branch (no skill copy)");
    } else {
        const uint32_t atk_skill = jh_player_person_skill_id_u32(attacker);
        if (atk_skill != 0u) {
            (void) CounterState::try_record_original_skill(vic_eff);
            jh_player_set_person_skill_id_u32(vic_eff, atk_skill);
            void *cached = jh_player_cached_skill(vic_eff);
            if (cached && GameHooks::fn_skill_init_for_player)
                (void) GameHooks::fn_skill_init_for_player(cached, vic_eff, atk_skill);
            if (GameHooks::fn_get_battle_head && GameHooks::fn_battle_head_change_skill) {
                void *head = GameHooks::fn_get_battle_head(vic_eff);
                if (head)
                    GameHooks::fn_battle_head_change_skill(head, atk_skill);
            }
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[CounterRetaliate] skillMirror vicPid=%d atkSkill=%u",
                    jh_player_person_id(vic_eff),
                    atk_skill);
        }
    }

    CounterState::mark_counter_attack_action(vic_eff);

    void *mgr = jh_player_battle_mgr(vic_eff);
    if (mgr && GameHooks::fn_fight_insert) {
        GameHooks::fn_fight_insert(mgr, vic_eff);
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[CounterRetaliate] fightInsert vicPid=%d",
                jh_player_person_id(vic_eff));
    }
}

static void hooked_add_hp(
        void *victim_this,
        void *attacker,
        int hp_amt,
        int dmg_type,
        bool a5,
        bool a6,
        void *vec2_indirect) {
    if (!g_orig_add_hp)
        return;

    if (!victim_this) {
        g_orig_add_hp(nullptr, attacker, hp_amt, dmg_type, a5, a6, vec2_indirect);
        return;
    }

    void *bm_v = jh_player_battle_mgr(victim_this);
    if (bm_v)
        g_last_battle_mgr_for_portrait = bm_v;
    if (attacker) {
        void *bm_a = jh_player_battle_mgr(attacker);
        if (bm_a)
            g_last_battle_mgr_for_portrait = bm_a;
    }

    ModConfusion::confusion_facing_before_addhp_cleanup(attacker, hp_amt);

    void *vic_eff = victim_this;
    const unsigned hp_u = static_cast<unsigned>(hp_amt);

    if (attacker && attacker != victim_this && (hp_u & 0x80000000u) != 0u &&
        ModConfusion::is_active_for_player(attacker) && !jh_player_same_side(attacker, victim_this)) {
        void *mgr = jh_player_battle_mgr(attacker);
        void *picked = ModConfusion::pick_confusion_substitute_external(attacker, nullptr, mgr);
        if (picked && picked != attacker && picked != victim_this) {
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[HUNLUAN] addHp remap atk=%d vicRaw=%d vicEff=%d amt=%u",
                    jh_player_person_id(attacker),
                    jh_player_person_id(victim_this),
                    jh_player_person_id(picked),
                    hp_u);
            vic_eff = picked;
        }
    }

    if (attacker && vic_eff != victim_this && (hp_u & 0x80000000u) != 0u)
        ModAddHpExtras::trace_hunluan_add_hp_if_remapped(victim_this, vic_eff, attacker, hp_amt, dmg_type);

    if (attacker && (hp_u & 0x80000000u) != 0u && ModConfusion::is_active_for_player(attacker) &&
        jh_player_same_side(attacker, vic_eff))
        ModConfusion::confusion_facing_apply_external(attacker, vic_eff, "addHp-pre");

    const int hp_before = jh_player_hp_s32(vic_eff);

    g_orig_add_hp(vic_eff, attacker, hp_amt, dmg_type, a5, a6, vec2_indirect);
    ModAddHpExtras::after_native_addhp_chunk(vic_eff, hp_amt);

    if (attacker && attacker != vic_eff && (hp_u & 0x80000000u) != 0u &&
        ModConfusion::is_active_for_player(attacker) && jh_player_same_side(attacker, vic_eff) &&
        jh_player_hp_s32(vic_eff) == hp_before && hp_before >= 1) {
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[HUNLUAN] addHp retry atk=%d vicEff=%d amt=%u",
                jh_player_person_id(attacker),
                jh_player_person_id(vic_eff),
                hp_u);
        g_orig_add_hp(vic_eff, attacker, hp_amt, dmg_type, a5, a6, vec2_indirect);
        ModAddHpExtras::after_native_addhp_chunk(vic_eff, hp_amt);
    }

    const int hp_after = jh_player_hp_s32(vic_eff);

    // 混乱误伤己方：addHp-pre 已翻面；若此处立刻 addHp-post 还原，朝向在同一帧内抵消，画面上几乎永不转向。
    const bool confused_allied_damage =
            attacker && (hp_u & 0x80000000u) != 0u && ModConfusion::is_active_for_player(attacker) &&
            jh_player_same_side(attacker, vic_eff);
    if (!confused_allied_damage)
        ModConfusion::confusion_facing_after_addhp_restore(attacker);
    else
        ModConfusion::confusion_facing_schedule_extend_after_confused_allied_damage();

    ModAddHpExtras::trace_add_hp_confusion_conditional(
            victim_this, vic_eff, attacker, hp_amt, dmg_type, hp_before, hp_after);

    ModAddHpExtras::finalize_after_confusion_traces(
            victim_this, vic_eff, attacker, hp_amt, dmg_type, a5, a6, hp_before, hp_after);

    if (attacker && vic_eff != attacker && (hp_u & 0x80000000u) != 0u) {
        add_hp_maybe_counter_retaliate(attacker, vic_eff, hp_u);
        ModAddHpExtras::maybe_derivative_qingying_counter_on_damage(attacker, vic_eff, hp_u);
    }
}

/// 与 `hooked_addHp` 受害侧 remap 一致：TLS 出手宿主混乱且 buff 落在敌对活人时，改挂到 pick 的同阵营替身。
static void *resolve_confusion_buff_apply_target(void *buffer_recv_jh) {
    if (!buffer_recv_jh)
        return nullptr;
    void *attacker = g_tls_skill_actor;
    if (!attacker || attacker == buffer_recv_jh)
        return buffer_recv_jh;
    if (!ModConfusion::is_active_for_player(attacker) || jh_player_same_side(attacker, buffer_recv_jh))
        return buffer_recv_jh;
    void *mgr = jh_player_battle_mgr(attacker);
    void *picked = ModConfusion::pick_confusion_substitute_external(attacker, nullptr, mgr);
    if (picked && picked != attacker && picked != buffer_recv_jh)
        return picked;
    return buffer_recv_jh;
}

static int64_t hooked_add_buffer_buff(void *jh_this, int buff_id, void *buff_ptr) {
    if (!g_orig_add_buffer_buff)
        return 0;
    void *target = resolve_confusion_buff_apply_target(jh_this);
    if (target != jh_this) {
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[HUNLUAN] addBuffer(Buff*) remap recvPid=%d -> effPid=%d buffId=%d tlsAtk=%d",
                jh_player_person_id(jh_this),
                jh_player_person_id(target),
                buff_id,
                g_tls_skill_actor ? jh_player_person_id(g_tls_skill_actor) : -1);
    }
    return g_orig_add_buffer_buff(target, buff_id, buff_ptr);
}

static int64_t hooked_add_buffer_buff_tr(void *jh_this, int buff_id, void *buff_tr, int a4, int a5) {
    if (!g_orig_add_buffer_buff_tr)
        return 0;
    void *target = resolve_confusion_buff_apply_target(jh_this);
    if (target != jh_this) {
        CAM_LOG(
                ANDROID_LOG_ERROR,
                "[HUNLUAN] addBuffer(BuffTr*) remap recvPid=%d -> effPid=%d buffId=%d tlsAtk=%d",
                jh_player_person_id(jh_this),
                jh_player_person_id(target),
                buff_id,
                g_tls_skill_actor ? jh_player_person_id(g_tls_skill_actor) : -1);
    }
    return g_orig_add_buffer_buff_tr(target, buff_id, buff_tr, a4, a5);
}

static int64_t hooked_on_round_begin(void *jh_this) {
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[TraceRoundBegin] tlsSkillActorBeforeClear=%p",
            g_tls_skill_actor);
    ModConfusion::clear_confusion_shouji_substitute_tls_cache();
    // 上一行动者混乱翻面可能跨多段伤害延迟还原；新角色回合开始时统一清理，避免立绘永久镜像。
    ModConfusion::confusion_facing_restore_external("onRoundBegin");
    g_tls_skill_actor = nullptr;

    if (!g_orig_on_round_begin)
        return 0;

    if (!jh_this)
        return g_orig_on_round_begin(nullptr);

    const int actor_pid = jh_player_person_id(jh_this);
    const int fight_round = CounterState::current_round_for_trace();
    const uint32_t skill_id = jh_player_person_skill_id_u32(jh_this);
    const uint32_t neigong_id = jh_player_person_neigong_id_u32(jh_this);
    const int hp = jh_player_hp_s32(jh_this);
    const uint32_t max_hp = jh_player_max_hp_u32(jh_this);

    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[TraceRoundBegin] actorPid=%d fightRound=%d skill=%u neigong=%u hp=%d/%u modConfused=%d tlsCleared=1",
            actor_pid,
            fight_round,
            skill_id,
            neigong_id,
            hp,
            max_hp,
            ModConfusion::is_active_for_player(jh_this) ? 1 : 0);

    uint32_t restored_skill = 0;
    if (CounterState::handle_player_round_begin_restore(jh_this, &restored_skill)) {
        const uint32_t nid_now = jh_player_person_neigong_id_u32(jh_this);
        const int rid = jh_player_person_id(jh_this);
        if (nid_now == 4000u) {
            CAM_LOG(ANDROID_LOG_ERROR, "[Chaos4000] pid=%d originalSkill=%u (skip restore apply)", rid, restored_skill);
        } else {
            CAM_LOG(
                    ANDROID_LOG_ERROR,
                    "[RestoreApply] pid=%d originalSkill=%u",
                    rid,
                    restored_skill);
            jh_player_set_person_skill_id_u32(jh_this, restored_skill);
            void *cached = jh_player_cached_skill(jh_this);
            if (cached && GameHooks::fn_skill_init_for_player)
                (void) GameHooks::fn_skill_init_for_player(cached, jh_this, restored_skill);
            if (GameHooks::fn_get_battle_head && GameHooks::fn_battle_head_change_skill) {
                void *head = GameHooks::fn_get_battle_head(jh_this);
                if (head)
                    GameHooks::fn_battle_head_change_skill(head, restored_skill);
            }
        }
    }

    return g_orig_on_round_begin(jh_this);
}

static int64_t hooked_skill_on_attack_event(void *skill_this, const void *attack_event_string_ref) {
    if (!g_orig_skill_on_attack_event)
        return 0;

    void *prev = g_tls_skill_actor;
    void *owner = skill_owner_player(skill_this);
    g_tls_skill_actor = owner;
    CAM_LOG(ANDROID_LOG_ERROR, "[TraceSkillTLS] onAttackEvent owner=%p prevTls=%p", owner, prev);

    SkillFacingScratch scratch{};
    scratch_prepare_skill_mirror(skill_this, &scratch);

    const int64_t ret =
            g_orig_skill_on_attack_event(skill_this, attack_event_string_ref);

    scratch_restore_skill_mirror(&scratch);
    g_tls_skill_actor = prev;
    return ret;
}

static int64_t hooked_skill_begin_act(void *skill_this) {
    if (!g_orig_skill_begin_act)
        return 0;

    ModConfusion::clear_confusion_shouji_substitute_tls_cache();

    void *prev = g_tls_skill_actor;
    void *owner = skill_owner_player(skill_this);
    g_tls_skill_actor = owner;
    CAM_LOG(ANDROID_LOG_ERROR, "[TraceSkillTLS] beginAct owner=%p prevTls=%p", owner, prev);

    SkillFacingScratch scratch{};
    scratch_prepare_skill_mirror(skill_this, &scratch);

    const int64_t ret = g_orig_skill_begin_act(skill_this);

    ModAddHpExtras::maybe_derivative_jinhuang_extra_turn_after_begin_act(owner);

    scratch_restore_skill_mirror(&scratch);
    g_tls_skill_actor = prev;
    return ret;
}

/// libnative `GaoyangMod::CounterAttackMod::BattleHead_updateBuff_hook`：战斗中 Buff UI 补丁（BattleHead+720 → JhPlayer，+42*sizeof(void*) 处字节）。
static int64_t hooked_battle_head_update_buff(void *battle_head_this) {
    if (!g_orig_battle_head_update_buff)
        return 0;
    if (!battle_head_this)
        return g_orig_battle_head_update_buff(nullptr);

    void **slot720 =
            reinterpret_cast<void **>(static_cast<char *>(battle_head_this) + 720);
    void *jh_player = slot720 ? *slot720 : nullptr;

    uint8_t *ui_patch = nullptr;
    uint8_t byte5_backup = 0;
    bool toggled = false;

    if (jh_player) {
        ui_patch =
                reinterpret_cast<uint8_t *>(static_cast<char *>(jh_player) + 42 * sizeof(void *));
        if (ModConfusion::is_active_for_player(jh_player) && ui_patch) {
            const uint8_t b4 = ui_patch[4];
            const uint8_t b5 = ui_patch[5];
            byte5_backup = b5;
            if (!b4 && !b5) {
                ui_patch[5] = 1;
                toggled = true;
                CAM_LOG(
                        ANDROID_LOG_ERROR,
                        "[TraceBuffUi] modConfusionUiPatch pid=%d fightRound=%d",
                        jh_player_person_id(jh_player),
                        ModConfusion::fight_round_snapshot());
            }
        }
    }

    const int64_t ret = g_orig_battle_head_update_buff(battle_head_this);
    if (toggled && ui_patch)
        ui_patch[5] = byte5_backup;
    return ret;
}

static void hooked_director_draw_scene(void *director_this) {
    if (g_orig_director_draw_scene)
        g_orig_director_draw_scene(director_this);
    ModConfusion::confusion_facing_tick_extend_after_draw_scene();
    ModAddHpExtras::flush_deferred_derivative_portrait_if_needed();
}

/// libnative `sub_A6214`：`beenAttack` 原版返回后调度 Derivative 立绘刷新（BattleHead+720 → JhPlayer）。
static int64_t hooked_battle_head_been_attack(void *battle_head_this, bool flag) {
    if (!g_orig_battle_head_been_attack)
        return 0;
    const int64_t ret = g_orig_battle_head_been_attack(battle_head_this, flag);
    ModAddHpExtras::derivative_notify_battle_head_been_attack_post(battle_head_this);
    return ret;
}

/// libnative `sub_A6310`：`onLastShouJiFrameImpl` 同上。
static int64_t hooked_battle_head_last_shouji_impl(void *battle_head_this, float dt) {
    if (!g_orig_battle_head_last_shouji_impl)
        return 0;
    const int64_t ret = g_orig_battle_head_last_shouji_impl(battle_head_this, dt);
    ModAddHpExtras::derivative_notify_battle_head_last_shouji_post(battle_head_this);
    return ret;
}

/// libnative `sub_A63D4`：战后清理混淆状态与技能 TLS。
static int64_t hooked_battle_on_end_battle(void *battle_this, float dt) {
    int64_t ret = 0;
    if (g_orig_battle_on_end_battle)
        ret = g_orig_battle_on_end_battle(battle_this, dt);
    // 原版返回后 BattleMgr 可能已释放；务必清空，否则 drawScene→flush→selectAllPlayers/getSurviveList 野指针崩溃。
    counter_hooks_set_last_battle_mgr_for_portrait(nullptr);
    ModConfusion::clear_all();
    CounterState::clear_session();
    ModAddHpExtras::on_battle_end();
    ModConfusion::confusion_facing_restore_external("onEndBattle");
    g_tls_skill_actor = nullptr;
    CAM_LOG(ANDROID_LOG_ERROR, "[TraceBattle] onEndBattle cleared confusion + skillTls battle=%p", battle_this);
    return ret;
}

} // namespace

void *counter_hooks_tls_skill_actor_ptr() {
    return g_tls_skill_actor;
}

void *counter_hooks_last_battle_mgr_for_portrait_ptr() {
    return g_last_battle_mgr_for_portrait;
}

void counter_hooks_set_last_battle_mgr_for_portrait(void *battle_mgr_or_null) {
    g_last_battle_mgr_for_portrait = battle_mgr_or_null;
}

void counter_hooks_install(uintptr_t cocos_elf_bias) {
    GameHooks::resolve(cocos_elf_bias);

    void *p_add_hp = reinterpret_cast<void *>(cocos_elf_bias + kAddHp);
    int rc = DobbyHook(
            p_add_hp,
            reinterpret_cast<dobby_dummy_func_t>(hooked_add_hp),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_add_hp));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] addHp rc=%d hook=%p orig=%p",
            rc,
            p_add_hp,
            reinterpret_cast<void *>(g_orig_add_hp));

    void *p_round_begin = reinterpret_cast<void *>(cocos_elf_bias + kOnRoundBegin);
    rc = DobbyHook(
            p_round_begin,
            reinterpret_cast<dobby_dummy_func_t>(hooked_on_round_begin),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_on_round_begin));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] onRoundBegin rc=%d hook=%p orig=%p",
            rc,
            p_round_begin,
            reinterpret_cast<void *>(g_orig_on_round_begin));

    void *p_skill_attack = reinterpret_cast<void *>(cocos_elf_bias + kSkillOneTime_onAttackEvent);
    rc = DobbyHook(
            p_skill_attack,
            reinterpret_cast<dobby_dummy_func_t>(hooked_skill_on_attack_event),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_skill_on_attack_event));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] SkillOneTime::onAttackEvent rc=%d orig=%p",
            rc,
            reinterpret_cast<void *>(g_orig_skill_on_attack_event));

    void *p_skill_begin = reinterpret_cast<void *>(cocos_elf_bias + kSkillOneTime_beginAct);
    rc = DobbyHook(
            p_skill_begin,
            reinterpret_cast<dobby_dummy_func_t>(hooked_skill_begin_act),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_skill_begin_act));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] SkillOneTime::beginAct rc=%d orig=%p",
            rc,
            reinterpret_cast<void *>(g_orig_skill_begin_act));

    void *p_add_buf = reinterpret_cast<void *>(cocos_elf_bias + kJhPlayer_addBuffer_Buff);
    rc = DobbyHook(
            p_add_buf,
            reinterpret_cast<dobby_dummy_func_t>(hooked_add_buffer_buff),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_add_buffer_buff));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] JhPlayer::addBuffer(Buff*) rc=%d hook=%p orig=%p",
            rc,
            p_add_buf,
            reinterpret_cast<void *>(g_orig_add_buffer_buff));

    void *p_add_buf_tr = reinterpret_cast<void *>(cocos_elf_bias + kJhPlayer_addBuffer_BuffTr);
    rc = DobbyHook(
            p_add_buf_tr,
            reinterpret_cast<dobby_dummy_func_t>(hooked_add_buffer_buff_tr),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_add_buffer_buff_tr));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] JhPlayer::addBuffer(BuffTr*) rc=%d hook=%p orig=%p",
            rc,
            p_add_buf_tr,
            reinterpret_cast<void *>(g_orig_add_buffer_buff_tr));

    void *p_bh_update_buff = reinterpret_cast<void *>(cocos_elf_bias + kBattleHead_updateBuff);
    rc = DobbyHook(
            p_bh_update_buff,
            reinterpret_cast<dobby_dummy_func_t>(hooked_battle_head_update_buff),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_battle_head_update_buff));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] BattleHead::updateBuff rc=%d hook=%p orig=%p",
            rc,
            p_bh_update_buff,
            reinterpret_cast<void *>(g_orig_battle_head_update_buff));

    void *p_bh_been_attack = reinterpret_cast<void *>(cocos_elf_bias + kBattleHead_beenAttack);
    rc = DobbyHook(
            p_bh_been_attack,
            reinterpret_cast<dobby_dummy_func_t>(hooked_battle_head_been_attack),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_battle_head_been_attack));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] BattleHead::beenAttack rc=%d hook=%p orig=%p",
            rc,
            p_bh_been_attack,
            reinterpret_cast<void *>(g_orig_battle_head_been_attack));

    void *p_bh_last_shouji = reinterpret_cast<void *>(cocos_elf_bias + kBattleHead_onLastShouJiFrameImpl);
    rc = DobbyHook(
            p_bh_last_shouji,
            reinterpret_cast<dobby_dummy_func_t>(hooked_battle_head_last_shouji_impl),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_battle_head_last_shouji_impl));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] BattleHead::onLastShouJiFrameImpl rc=%d hook=%p orig=%p",
            rc,
            p_bh_last_shouji,
            reinterpret_cast<void *>(g_orig_battle_head_last_shouji_impl));

    void *p_battle_end = reinterpret_cast<void *>(cocos_elf_bias + kBattle_onEndBattle);
    rc = DobbyHook(
            p_battle_end,
            reinterpret_cast<dobby_dummy_func_t>(hooked_battle_on_end_battle),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_battle_on_end_battle));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] Battle::onEndBattle rc=%d hook=%p orig=%p",
            rc,
            p_battle_end,
            reinterpret_cast<void *>(g_orig_battle_on_end_battle));

    void *p_draw_scene = reinterpret_cast<void *>(cocos_elf_bias + kDirector_drawScene);
    rc = DobbyHook(
            p_draw_scene,
            reinterpret_cast<dobby_dummy_func_t>(hooked_director_draw_scene),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_director_draw_scene));
    CAM_LOG(
            ANDROID_LOG_ERROR,
            "[HookInstall] Director::drawScene rc=%d hook=%p orig=%p",
            rc,
            p_draw_scene,
            reinterpret_cast<void *>(g_orig_director_draw_scene));
}

//蛇咬是爹
