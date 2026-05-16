#pragma once

#include <cstdint>

namespace GameHooks {

void resolve(uintptr_t cocos_elf_bias);

/// BattleMgr::selectAllPlayers(bool, std::vector<JhPlayer *> &) — lst: `.text:00000000004FDEE0`
extern void (*fn_select_all_players)(void *battle_mgr, bool side_nonzero, void *vector_abi_this);

extern void *(*fn_get_battle_head)(void *jh_player);

extern float (*fn_get_scale_x)(void *node);

extern void (*fn_set_scale_x)(void *node, float x);

extern float (*fn_get_rotation_y)(void *node);

extern void (*fn_set_rotation_y)(void *node, float y);

// JhPlayer::onNewFightRound(JhPlayer*, int) — lst: `.text:00000000005CFED4` `_ZN8JhPlayer15onNewFightRoundEi`
using JhPlayer_onNewFightRound_t = int64_t (*)(void *jh_player_this, int fight_round_param);
extern JhPlayer_onNewFightRound_t orig_on_new_fight_round;

// JhPlayer::getActShouJiPlayer(void) — lst: `.text:00000000005CBA04` `_ZN8JhPlayer18getActShouJiPlayerEv`
using JhPlayer_getActShouJiPlayer_t = void * (*)(void *jh_player_this);
extern JhPlayer_getActShouJiPlayer_t orig_get_act_shouji_player;

extern void (*fn_fight_insert)(void *battle_mgr, void *jh_player);

extern void (*fn_battle_head_change_skill)(void *battle_head, uint32_t skill_id);

/// SkillBase::init(JhPlayer*, int) — lst `.text:000000000065513C` `_ZN9SkillBase4initEP8JhPlayeri`
extern void *(*fn_skill_init_for_player)(void *skill_base_this, void *jh_player, uint32_t skill_id);

// ---- libnative `GaoyangMod::CounterAttackMod::InitializeHooks` 扩展（小数 RVA = InitializeHooks 中原值）----
extern void (*fn_sub_engine_string_from_utf8)(void **out_user_string_slot, const char *utf8_z);
extern void (*fn_person_cfg_assign_display_from_engine_str)(uintptr_t person_cfg_plus16, void *engine_str_ref);
extern void (*fn_snapshot_person_tr_nick)(void *out_qword, uintptr_t person_cfg_plus16);
extern void (*fn_engine_ctrl_release)(void *ctrl_block_ptr);
extern uintptr_t addr_engine_string_empty_control_block;

extern void (*fn_battle_head_update_head)(void *battle_head);
extern void (*fn_battle_head_fu_huo)(void *battle_head, double zero);

/// JhSetSpriteFrame(Node*, EngineString&) — lst / InitializeHooks +5629092
extern void (*fn_jh_set_sprite_frame)(void *node, void **engine_string_ref_slot);

extern void *(*fn_delay_time_create)(float seconds);
extern void *(*fn_fade_out_create)(void *unused_null, float duration);
extern void *(*fn_sequence_two_actions)(void *first_action, void *second_action);
extern void (*fn_node_run_action)(void *node, void *action);

extern void (*fn_node_set_visible)(void *node, long visible);
extern void (*fn_node_set_opacity)(void *node, long opacity255);
extern void *(*fn_node_get_child_by_name)(void *node, void **engine_string_ref);

/// cocos2d::Node::getParent(void) — lst `.text:00000000004E7AA4` `_ZN7cocos2d4Node9getParentEv`
extern void *(*fn_node_get_parent)(void *node);

/// cocos2d::Node::setCascadeOpacityEnabled(bool) — lst `.text:00000000008879A8`
extern void (*fn_node_set_cascade_opacity_enabled)(void *node, bool enabled);

/// cocos2d::Node::setColor(Color3B const&) — lst `.text:00000000008865F4`
extern void (*fn_node_set_color_rgb)(void *node, const unsigned char *rgb3);

} // namespace GameHooks
//蛇咬爹