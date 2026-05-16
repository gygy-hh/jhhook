#pragma once

#include <cstdint>

namespace ModConfusion {

void install_add_buffer_hook_if_needed();

void on_new_fight_round(int round_param);

void apply(void *actor_as_dict_key, intptr_t duration_rounds_disguised);

void apply_overwrite(void *actor_as_dict_key, intptr_t duration_rounds_disguised);

void clear_actor(void *actor_as_dict_key);

void clear_all();

bool is_active_for_player(void *jh_player);

/// 当前快照的回合计数（来自 `onNewFightRound` 回调参数）；对齐 BattleHead_updateBuff 等 trace 中的 fightRound。
int fight_round_snapshot();

/// addHp 入口：对齐 libnative `qword_14F8F0` 前置清理语义（映射到本模组 thread_local 朝向包）。
void confusion_facing_before_addhp_cleanup(void *attacker_jh_player, int hp_amt);

/// addHp 末尾：仅当当前 TLS 记录的攻击者与本次 attacker 一致时恢复朝向。
void confusion_facing_after_addhp_restore(void *attacker_jh_player);

void confusion_facing_restore_external(const char *reason_tag);

/// 混乱误伤己方且跳过立即 addHp-post 时刷新：最后一跳伤害后再维持若干 drawScene 帧再还原朝向（不改数值/remap）。
void confusion_facing_schedule_extend_after_confused_allied_damage();

void confusion_facing_tick_extend_after_draw_scene();

void confusion_facing_apply_external(void *attacker, void *target, const char *tag);

void *pick_confusion_substitute_external(void *attacker, void *preferred_target_or_null, void *battle_mgr);

/// 新开招式或回合时清空：否则混乱替身多次 rand，`getActShouJiPlayer`/伤害目标在同一招内会乱跳。
void clear_confusion_shouji_substitute_tls_cache();

/// 安装 Dobby：onNewFightRound / getActShouJiPlayer，并解析依赖的游戏函数指针。
void install_confusion_hooks(uintptr_t cocos_elf_bias);

} // namespace ModConfusion
//蛇咬爹