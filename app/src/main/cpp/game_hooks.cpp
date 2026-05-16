#include "game_hooks.hpp"

namespace {

// RVAs = libcocos2dcpp.so.lst `.text:0000000000.......`（与 libcocos2dcp.c 中标注的 VA 一致；典型 ET_DYN 基址 0）。
constexpr uintptr_t kSelectAllPlayers = 0x4FDEE0u;   // BattleMgr::selectAllPlayers
constexpr uintptr_t kGetBattleHead = 0x5CAD34u;      // JhPlayer::getBattleHead
constexpr uintptr_t kSetScaleX = 0x885F98u;          // cocos2d::Node::setScaleX(float)
constexpr uintptr_t kGetScaleX = 0x885F5Cu;          // cocos2d::Node::getScaleX(void) const
constexpr uintptr_t kSetRotationY = 0x4E7A54u;       // cocos2d::Node::setRotationY(float)
constexpr uintptr_t kGetRotationY = 0x4E7A74u;       // cocos2d::Node::getRotationY(void) const

constexpr uintptr_t kOnNewFightRound = 0x5CFED4u;    // JhPlayer::onNewFightRound(int)
constexpr uintptr_t kGetActShouJiPlayer = 0x5CBA04u; // JhPlayer::getActShouJiPlayer(void)

// CounterAttackMod::InitializeHooks 中与还击链相关的游戏内函数（偏移相对 libcocos2dcpp.so 基址）。
constexpr uintptr_t kFightInsert = 5229024u;
constexpr uintptr_t kBattleHeadChangeSkill = 5225496u;
constexpr uintptr_t kSkillBaseInit = 0x65513Cu; // SkillBase::init(JhPlayer*, int)

// InitializeHooks 小数 RVA（相对 libcocos2dcpp.so 映射基址）
constexpr uintptr_t kBattleHeadUpdateHead = 5218320u;
constexpr uintptr_t kBattleHeadFuHuo = 5218964u;
constexpr uintptr_t kSubEngineStringUtf8 = 13049444u;
constexpr uintptr_t kSubPersonCfgDisplayAssign = 13046992u;
constexpr uintptr_t kSubSnapshotPersonTrNick = 13046872u;
constexpr uintptr_t kSubEngineCtrlRelease = 6268116u;
constexpr uintptr_t kEngineStringEmptyControlOffset = 17324256u;
constexpr uintptr_t kDelayTimeCreate = 8749784u;
constexpr uintptr_t kFadeOutCreate = 8747896u;
constexpr uintptr_t kSequenceTwoActions = 8734848u;
constexpr uintptr_t kNodeRunAction = 8942060u;
constexpr uintptr_t kNodeSetVisible = 8937788u;
constexpr uintptr_t kNodeSetOpacity = 8938672u;
constexpr uintptr_t kNodeGetChildByName = 8949828u;
constexpr uintptr_t kJhSetSpriteFrame = 5629092u;
constexpr uintptr_t kNodeGetParent = 0x4E7AA4u;                // Node::getParent
constexpr uintptr_t kNodeSetCascadeOpacityEnabled = 0x8879A8u; // Node::setCascadeOpacityEnabled(bool)
constexpr uintptr_t kNodeSetColorRgb = 0x8865F4u;             // Node::setColor(Color3B const&)

} // namespace

namespace GameHooks {

void (*fn_select_all_players)(void *, bool, void *) = nullptr;

void *(*fn_get_battle_head)(void *) = nullptr;

float (*fn_get_scale_x)(void *) = nullptr;

void (*fn_set_scale_x)(void *, float) = nullptr;

float (*fn_get_rotation_y)(void *) = nullptr;

void (*fn_set_rotation_y)(void *, float) = nullptr;

JhPlayer_onNewFightRound_t orig_on_new_fight_round = nullptr;

JhPlayer_getActShouJiPlayer_t orig_get_act_shouji_player = nullptr;

void (*fn_fight_insert)(void *battle_mgr, void *jh_player) = nullptr;

void (*fn_battle_head_change_skill)(void *battle_head, uint32_t skill_id) = nullptr;

void *(*fn_skill_init_for_player)(void *skill_base_this, void *jh_player, uint32_t skill_id) = nullptr;

void (*fn_sub_engine_string_from_utf8)(void **out_user_string_slot, const char *utf8_z) = nullptr;

void (*fn_person_cfg_assign_display_from_engine_str)(uintptr_t person_cfg_plus16,
        void *engine_str_ref) = nullptr;

void (*fn_snapshot_person_tr_nick)(void *out_qword, uintptr_t person_cfg_plus16) = nullptr;

void (*fn_engine_ctrl_release)(void *ctrl_block_ptr) = nullptr;

uintptr_t addr_engine_string_empty_control_block = 0;

void (*fn_battle_head_update_head)(void *battle_head) = nullptr;

void (*fn_battle_head_fu_huo)(void *battle_head, double zero) = nullptr;

void *(*fn_delay_time_create)(float seconds) = nullptr;

void *(*fn_fade_out_create)(void *unused_null, float duration) = nullptr;

void *(*fn_sequence_two_actions)(void *first_action, void *second_action) = nullptr;

void (*fn_node_run_action)(void *node, void *action) = nullptr;

void (*fn_node_set_visible)(void *node, long visible) = nullptr;

void (*fn_node_set_opacity)(void *node, long opacity255) = nullptr;

void *(*fn_node_get_child_by_name)(void *node, void **engine_string_ref) = nullptr;

void *(*fn_node_get_parent)(void *node) = nullptr;

void (*fn_node_set_cascade_opacity_enabled)(void *node, bool enabled) = nullptr;

void (*fn_node_set_color_rgb)(void *node, const unsigned char *rgb3) = nullptr;

void (*fn_jh_set_sprite_frame)(void *node, void **engine_string_ref_slot) = nullptr;

void resolve(uintptr_t base) {
    fn_select_all_players =
            reinterpret_cast<decltype(fn_select_all_players)>(base + kSelectAllPlayers);
    fn_get_battle_head =
            reinterpret_cast<decltype(fn_get_battle_head)>(base + kGetBattleHead);
    fn_set_scale_x =
            reinterpret_cast<decltype(fn_set_scale_x)>(base + kSetScaleX);
    fn_get_scale_x =
            reinterpret_cast<decltype(fn_get_scale_x)>(base + kGetScaleX);
    fn_set_rotation_y =
            reinterpret_cast<decltype(fn_set_rotation_y)>(base + kSetRotationY);
    fn_get_rotation_y =
            reinterpret_cast<decltype(fn_get_rotation_y)>(base + kGetRotationY);
    orig_on_new_fight_round =
            reinterpret_cast<JhPlayer_onNewFightRound_t>(base + kOnNewFightRound);
    orig_get_act_shouji_player =
            reinterpret_cast<JhPlayer_getActShouJiPlayer_t>(base + kGetActShouJiPlayer);
    fn_fight_insert = reinterpret_cast<decltype(fn_fight_insert)>(base + kFightInsert);
    fn_battle_head_change_skill =
            reinterpret_cast<decltype(fn_battle_head_change_skill)>(base + kBattleHeadChangeSkill);
    fn_skill_init_for_player =
            reinterpret_cast<decltype(fn_skill_init_for_player)>(base + kSkillBaseInit);

    fn_sub_engine_string_from_utf8 = reinterpret_cast<decltype(fn_sub_engine_string_from_utf8)>(
            base + kSubEngineStringUtf8);
    fn_person_cfg_assign_display_from_engine_str =
            reinterpret_cast<decltype(fn_person_cfg_assign_display_from_engine_str)>(
                    base + kSubPersonCfgDisplayAssign);
    fn_snapshot_person_tr_nick =
            reinterpret_cast<decltype(fn_snapshot_person_tr_nick)>(base + kSubSnapshotPersonTrNick);
    fn_engine_ctrl_release =
            reinterpret_cast<decltype(fn_engine_ctrl_release)>(base + kSubEngineCtrlRelease);
    addr_engine_string_empty_control_block = base + kEngineStringEmptyControlOffset;

    fn_battle_head_update_head =
            reinterpret_cast<decltype(fn_battle_head_update_head)>(base + kBattleHeadUpdateHead);
    fn_battle_head_fu_huo =
            reinterpret_cast<decltype(fn_battle_head_fu_huo)>(base + kBattleHeadFuHuo);

    fn_delay_time_create =
            reinterpret_cast<decltype(fn_delay_time_create)>(base + kDelayTimeCreate);
    fn_fade_out_create = reinterpret_cast<decltype(fn_fade_out_create)>(base + kFadeOutCreate);
    fn_sequence_two_actions =
            reinterpret_cast<decltype(fn_sequence_two_actions)>(base + kSequenceTwoActions);
    fn_node_run_action = reinterpret_cast<decltype(fn_node_run_action)>(base + kNodeRunAction);

    fn_node_set_visible = reinterpret_cast<decltype(fn_node_set_visible)>(base + kNodeSetVisible);
    fn_node_set_opacity = reinterpret_cast<decltype(fn_node_set_opacity)>(base + kNodeSetOpacity);
    fn_node_get_child_by_name =
            reinterpret_cast<decltype(fn_node_get_child_by_name)>(base + kNodeGetChildByName);
    fn_jh_set_sprite_frame =
            reinterpret_cast<decltype(fn_jh_set_sprite_frame)>(base + kJhSetSpriteFrame);
    fn_node_get_parent = reinterpret_cast<decltype(fn_node_get_parent)>(base + kNodeGetParent);
    fn_node_set_cascade_opacity_enabled =
            reinterpret_cast<decltype(fn_node_set_cascade_opacity_enabled)>(
                    base + kNodeSetCascadeOpacityEnabled);
    fn_node_set_color_rgb =
            reinterpret_cast<decltype(fn_node_set_color_rgb)>(base + kNodeSetColorRgb);
}

} // namespace GameHooks
//又给蛇咬爹尽孝了qaq