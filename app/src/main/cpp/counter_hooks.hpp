#pragma once

#include <cstdint>

/// SkillOneTime TLS（武学宿主）；对齐全版 `JhPlayer_addHp_hook`。
void *counter_hooks_tls_skill_actor_ptr();

/// 对齐 libnative `g_lastBattleMgrForPortrait`（addHp 钩子里按受击者/攻击者 BattleMgr 刷新）。
void *counter_hooks_last_battle_mgr_for_portrait_ptr();

/// BattleHead::beenAttack 后置：`sub_A6214` 更新 portrait mgr。
void counter_hooks_set_last_battle_mgr_for_portrait(void *battle_mgr_or_null);

/// CounterAttackMod（还击）中与 JhPlayer::addHp / onRoundBegin / SkillOneTime 相关的 Hook，
/// 与 libnative-lib.c InitializeHooks 中 RVA 对齐（绑定某一版本的 libcocos2dcpp.so）。
void counter_hooks_install(uintptr_t cocos_elf_bias);
///蛇咬爹