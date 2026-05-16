#pragma once

#include <cstdint>

namespace ModAddHpExtras {

/// 每次调用原版 `JhPlayer::addHp` 之后立即执行（对齐 libnative `sub_A4CE4` 前置调用）。
void after_native_addhp_chunk(void *victim_effective_jh, int hp_amt);

/// libnative 18978–18997：混乱目标改写后打印 `[TraceHunluanAddHp]`。
void trace_hunluan_add_hp_if_remapped(void *vic_raw_jh,
        void *vic_eff_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type);

/// libnative 19029–19063：受伤与攻击者不同且为伤害、且任一方处于混乱时打印完整 `[TraceAddHp]`。
void trace_add_hp_confusion_conditional(void *vic_raw_jh,
        void *vic_eff_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type,
        int hp_before_first_damage,
        int hp_after_final);

/// 对齐 libnative `JhPlayer_addHp_hook` 中原版 `addHp`/retry 之后、`BeginBattleHead…` 之前的收尾链。
void finalize_after_confusion_traces(void *vic_raw_jh,
        void *victim_effective_jh,
        void *attacker_jh,
        int hp_amt,
        int dmg_type,
        bool add_hp_arg_a5,
        bool add_hp_arg_a6,
        int hp_before_first_damage,
        int hp_after_final);

void on_battle_end();

/// 新战斗回合（与 CounterState 同步）：清空金煌「跳过下一次 beginAct 判定」等回合级标记。
void derivative_act_flags_on_new_fight_round();

/// 朱煞离场增幅到期：`fight_round_param >= expire` 时还原 JhPlayer 功力/速度加成（与混乱 expire 语义一致）。
void zhusha_amp_on_new_fight_round(int fight_round_param);

/// 青影（衍生 pers=102）：对齐内功还击节奏，每对攻击者首段伤害 50% 几率镜像对手武学并列队还击。
void maybe_derivative_qingying_counter_on_damage(void *attacker_jh, void *victim_effective_jh, unsigned hp_amt_unsigned);

/// 金煌（衍生 pers=103）：SkillOneTime::beginAct 结束后 70% 再 fightInsert 一次；连锁出的那次不再判定以免无限套娃。
void maybe_derivative_jinhuang_extra_turn_after_begin_act(void *jh_skill_owner);

/// libnative `FlushDeferredDerivativePortraitAtDrawSceneTail`：每帧 drawScene 尾部调用。
void flush_deferred_derivative_portrait_if_needed();

/// libnative `sub_A6214`：`BattleHead::beenAttack` 原版返回之后的 Derivative 肖像补偿。
void derivative_notify_battle_head_been_attack_post(void *battle_head_this);

/// libnative `sub_A6310`：`onLastShouJiFrameImpl` 原版返回之后的 Derivative 肖像调度。
void derivative_notify_battle_head_last_shouji_post(void *battle_head_this);

} // namespace ModAddHpExtras
//蛇咬爹