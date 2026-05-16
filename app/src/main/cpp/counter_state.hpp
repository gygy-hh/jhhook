#pragma once

#include <cstdint>

namespace CounterState {

/// 对齐 `GaoyangMod::StateManager::handleNewFightRound`：回合变化时清空「每对先手伤害」与「还击动作」集合。
void handle_new_fight_round(int round_param);

/// 对齐 `handlePlayerRoundBeginRestore`：若本体仍在还击标记集合则摘除并跳过；
/// 否则若存在记录的原技能则写入 `*out_original_skill` 并移除记录（返回 true）。
bool handle_player_round_begin_restore(void *jh_player, uint32_t *out_original_skill);

/// 对齐 `tryRecordOriginalSkill`：按人物 ID 首次记录当前技能（还击覆盖前调用）。
bool try_record_original_skill(void *jh_player);

/// 对齐 `isFirstDamageThisRound(attacker, victim)`（libnative 首参实为 attacker 指针）。
bool is_first_damage_this_round(void *attacker_jh, void *victim_jh);

/// 对齐 `markCounterAttackAction`：标记受害者处于还击动作流程（影响下一两次 roundBegin）。
void mark_counter_attack_action(void *victim_jh);

void clear_session();

int current_round_for_trace();

} // namespace CounterState
//蛇咬爹
