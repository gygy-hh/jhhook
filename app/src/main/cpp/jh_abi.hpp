#pragma once

// JhPlayer / JhPerson 字段偏移取自 Hex-Rays 反编译 libnative-lib.c（与 libcocos2dcpp.so 某一构建绑定）。
#include <cstdint>

inline void *jh_player_person(void *jh_player) {
    if (!jh_player)
        return nullptr;
    return *reinterpret_cast<void **>(static_cast<char *>(jh_player) + 64);
}

inline int jh_player_person_id(void *jh_player) {
    void *person = jh_player_person(jh_player);
    if (!person)
        return -1;
    return static_cast<int>(*reinterpret_cast<uint32_t *>(static_cast<char *>(person) + 32));
}

inline uint32_t jh_player_personality_id(void *jh_player) {
    void *person = jh_player_person(jh_player);
    if (!person)
        return 0;
    void *cfg = *reinterpret_cast<void **>(static_cast<char *>(person) + 64);
    if (!cfg)
        return 0;
    return *reinterpret_cast<uint32_t *>(static_cast<char *>(cfg) + 136);
}

inline uint32_t jh_player_hp_u32(void *jh_player) {
    if (!jh_player)
        return static_cast<uint32_t>(-1);
    return *reinterpret_cast<uint32_t *>(static_cast<char *>(jh_player) + 204);
}

inline int jh_player_hp_s32(void *jh_player) {
    return static_cast<int>(jh_player_hp_u32(jh_player));
}

inline uint32_t jh_player_max_hp_u32(void *jh_player) {
    if (!jh_player)
        return 0;
    return *reinterpret_cast<uint32_t *>(static_cast<char *>(jh_player) + 244);
}

inline uint32_t jh_player_person_skill_id_u32(void *jh_player) {
    void *person = jh_player_person(jh_player);
    if (!person)
        return 0;
    return *reinterpret_cast<uint32_t *>(static_cast<char *>(person) + 52);
}

inline void jh_player_set_person_skill_id_u32(void *jh_player, uint32_t skill_id) {
    void *person = jh_player_person(jh_player);
    if (!person)
        return;
    *reinterpret_cast<uint32_t *>(static_cast<char *>(person) + 52) = skill_id;
}

/// JhPerson::getNeigongId（libnative Hex-Rays：`*((unsigned int *)person + 11)`）
inline uint32_t jh_player_person_neigong_id_u32(void *jh_player) {
    void *person = jh_player_person(jh_player);
    if (!person)
        return 0;
    return *reinterpret_cast<uint32_t *>(static_cast<char *>(person) + 44);
}

inline void *jh_player_cached_skill(void *jh_player) {
    if (!jh_player)
        return nullptr;
    return *reinterpret_cast<void **>(static_cast<char *>(jh_player) + 72);
}

inline void *jh_player_battle_mgr(void *jh_player) {
    if (!jh_player)
        return nullptr;
    return *reinterpret_cast<void **>(static_cast<char *>(jh_player) + 48);
}

inline uint8_t jh_player_side_byte(void *jh_player) {
    if (!jh_player)
        return 0xFF;
    return *reinterpret_cast<uint8_t *>(static_cast<char *>(jh_player) + 61);
}

inline bool jh_player_same_side(void *a, void *b) {
    return a && b && jh_player_side_byte(a) == jh_player_side_byte(b);
}

/// libnative `JhPlayer::getMianShangModifier` / `setMianShangModifier` — 偏移 +308。
inline int32_t jh_player_mianshang_modifier_s32(void *jh_player) {
    if (!jh_player)
        return 0;
    return *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + 308);
}

inline void jh_player_set_mianshang_modifier_s32(void *jh_player, int32_t v) {
    if (!jh_player)
        return;
    *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + 308) = v;
}

inline void jh_player_set_hp_s32(void *jh_player, int32_t hp) {
    if (!jh_player)
        return;
    *reinterpret_cast<uint32_t *>(static_cast<char *>(jh_player) + 204) =
            static_cast<uint32_t>(hp);
}

inline void jh_player_set_revive_flag_s32(void *jh_player, int32_t v) {
    if (!jh_player)
        return;
    *reinterpret_cast<int32_t *>(static_cast<char *>(jh_player) + 224) = v;
}

inline void *jh_person_config_ptr(void *jh_person) {
    if (!jh_person)
        return nullptr;
    return *reinterpret_cast<void **>(static_cast<char *>(jh_person) + 64);
}

inline void jh_person_set_id_u32(void *jh_person, uint32_t id) {
    if (!jh_person)
        return;
    *reinterpret_cast<uint32_t *>(static_cast<char *>(jh_person) + 32) = id;
}
//蛇咬爹