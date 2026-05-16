#include "counter_state.hpp"

#include <android/log.h>
#include <pthread.h>

#include <map>
#include <set>
#include <utility>

#include "jh_abi.hpp"

#define MS_LOG(prio, ...) __android_log_print(prio, "ModStateManager", __VA_ARGS__)

namespace {

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int g_current_round_count = -1;

std::map<int, uint32_t> g_initial_skill_by_person_id;

std::set<uintptr_t> g_in_counter_attack_action;

std::set<std::pair<uintptr_t, uintptr_t>> g_damage_pairs_round;

} // namespace

namespace CounterState {

void handle_new_fight_round(int round_param) {
    pthread_mutex_lock(&g_mu);
    if (g_current_round_count == round_param) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_current_round_count = round_param;
    g_damage_pairs_round.clear();
    g_in_counter_attack_action.clear();
    pthread_mutex_unlock(&g_mu);
}

void clear_session() {
    pthread_mutex_lock(&g_mu);
    g_current_round_count = -1;
    g_initial_skill_by_person_id.clear();
    g_in_counter_attack_action.clear();
    g_damage_pairs_round.clear();
    pthread_mutex_unlock(&g_mu);
}

int current_round_for_trace() {
    pthread_mutex_lock(&g_mu);
    const int r = g_current_round_count;
    pthread_mutex_unlock(&g_mu);
    return r;
}

bool try_record_original_skill(void *jh_player) {
    if (!jh_player)
        return false;
    const int pid = jh_player_person_id(jh_player);
    if (pid < 0)
        return false;

    pthread_mutex_lock(&g_mu);
    if (g_initial_skill_by_person_id.find(pid) != g_initial_skill_by_person_id.end()) {
        pthread_mutex_unlock(&g_mu);
        return false;
    }
    const uint32_t sid = jh_player_person_skill_id_u32(jh_player);
    g_initial_skill_by_person_id.emplace(pid, sid);
    pthread_mutex_unlock(&g_mu);

    MS_LOG(ANDROID_LOG_ERROR, "[TryRecordOriginalSkill] pid=%d skill=%u", pid, sid);
    return true;
}

bool handle_player_round_begin_restore(void *jh_player, uint32_t *out_original_skill) {
    if (!jh_player || !out_original_skill)
        return false;

    const int pid = jh_player_person_id(jh_player);
    if (pid < 0)
        return false;

    pthread_mutex_lock(&g_mu);

    const uintptr_t ptr_k = reinterpret_cast<uintptr_t>(jh_player);
    if (g_in_counter_attack_action.erase(ptr_k) != 0u) {
        pthread_mutex_unlock(&g_mu);
        return false;
    }

    auto it = g_initial_skill_by_person_id.find(pid);
    if (it == g_initial_skill_by_person_id.end()) {
        pthread_mutex_unlock(&g_mu);
        return false;
    }

    const uint32_t saved = it->second;
    g_initial_skill_by_person_id.erase(it);
    pthread_mutex_unlock(&g_mu);

    *out_original_skill = saved;
    MS_LOG(ANDROID_LOG_ERROR, "[Restore] pid=%d originalSkill=%u", pid, saved);
    return true;
}

bool is_first_damage_this_round(void *attacker_jh, void *victim_jh) {
    if (!attacker_jh || !victim_jh)
        return false;

    const uintptr_t pa = reinterpret_cast<uintptr_t>(attacker_jh);
    const uintptr_t pv = reinterpret_cast<uintptr_t>(victim_jh);

    pthread_mutex_lock(&g_mu);
    const bool inserted =
            g_damage_pairs_round.insert(std::make_pair(pa, pv)).second;
    pthread_mutex_unlock(&g_mu);
    return inserted;
}

void mark_counter_attack_action(void *victim_jh) {
    if (!victim_jh)
        return;
    pthread_mutex_lock(&g_mu);
    g_in_counter_attack_action.insert(reinterpret_cast<uintptr_t>(victim_jh));
    pthread_mutex_unlock(&g_mu);
}

} // namespace CounterState
//蛇咬爹