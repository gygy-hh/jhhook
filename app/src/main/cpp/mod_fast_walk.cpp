#include "mod_fast_walk.hpp"

#include <android/log.h>
#include <cstdint>

#include "dobby.h"

#define FASTWALK_TAG "FastWalkMod"
#define FW_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, FASTWALK_TAG, __VA_ARGS__)

namespace {

// libcocos2dcpp.so.lst EXPORT（与仓库 lst 一致）
constexpr uintptr_t kMainScene2_onEnterGrid = 0x617168u; // MainScene2::onEnterGrid(int,int,int,int,bool)
constexpr uintptr_t kJhData_getPropCount = 0x56E114u;   // JhData::getPropCount(int)
constexpr uintptr_t kBss_s_jhData = 0xFD5B30u;          // bss EXPORT s_jhData（全局 JhData*）
// getPlayerLocation 的 fallback：从 g_info 链读出 grid（lst 0x5679E8..0x567A14），不在 hook 内传 std::string（跨 so ABI 会崩）。
constexpr uintptr_t kBss_g_info = 0xFD5B90u; // bss EXPORT g_info

constexpr int kStrideTiles = 5;
constexpr int kBootsPropId = 9110;

using MainScene2_onEnterGrid_t =
        int64_t (*)(void *main_scene_this, int from_or_meta_a, int from_or_meta_b, int to_grid_x, int to_grid_y,
                unsigned char flag_u8);

using JhData_getPropCount_t = int (*)(void *jh_data_this, int prop_id);

MainScene2_onEnterGrid_t g_orig_on_enter_grid = nullptr;

JhData_getPropCount_t g_fn_get_prop_count = nullptr;

uintptr_t g_cocos_bias = 0;

inline void *jh_data_singleton() {
    if (!g_cocos_bias)
        return nullptr;
    auto slot = reinterpret_cast<void **>(g_cocos_bias + kBss_s_jhData);
    return slot ? *slot : nullptr;
}

static inline int manhattan(int ax, int ay, int bx, int by) {
    const int dx = bx - ax;
    const int dy = by - ay;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    return adx + ady;
}

/// 与 lst 中 `*(**g)+8/+0xC` 一致：尝试两种指针层级（不同编译下 g_info 可能多包一层）。
static bool read_player_grid_for_adjacent_step(int to_x, int to_y, int *out_cx, int *out_cy) {
    if (!g_cocos_bias)
        return false;
    auto gslot = reinterpret_cast<void **>(g_cocos_bias + kBss_g_info);
    if (!gslot)
        return false;
    void *const p = *gslot;
    if (!p)
        return false;

    auto try_pair = [&](void *base) -> bool {
        if (!base)
            return false;
        auto *bytes = static_cast<char *>(base);
        const int32_t gx = *reinterpret_cast<int32_t *>(bytes + 8);
        const int32_t gy = *reinterpret_cast<int32_t *>(bytes + 12);
        if (manhattan(static_cast<int>(gx), static_cast<int>(gy), to_x, to_y) != 1)
            return false;
        *out_cx = static_cast<int>(gx);
        *out_cy = static_cast<int>(gy);
        return true;
    };

    // lst: LDR r,[g_info]; LDR r,[r]; 再读 [r+8],[r+0xC]
    void *inner = *reinterpret_cast<void **>(p);
    if (try_pair(inner))
        return true;
    return try_pair(p);
}

/// 仅处理曼哈顿距离为 1 的正交方向一步，目标改为沿同方向第 kStrideTiles 格。
static void boost_adjacent_destination(int cur_x, int cur_y, int *to_x, int *to_y) {
    const int tx = *to_x;
    const int ty = *to_y;
    const int dx = tx - cur_x;
    const int dy = ty - cur_y;

    if (dy == 0 && (dx == 1 || dx == -1))
        *to_x = cur_x + dx * kStrideTiles;
    else if (dx == 0 && (dy == 1 || dy == -1))
        *to_y = cur_y + dy * kStrideTiles;
}

static int64_t hooked_on_enter_grid(void *scene, int p1, int p2, int to_x, int to_y, unsigned char flag) {
    if (!g_orig_on_enter_grid)
        return 0;

    int nx = to_x;
    int ny = to_y;

    void *jh = jh_data_singleton();
    if (jh && g_fn_get_prop_count && g_fn_get_prop_count(jh, kBootsPropId) > 0) {
        int cx{};
        int cy{};
        bool have_cur = false;
        // 调用方传入的上一格坐标（部分路径会给出有效格子）
        if (p1 >= 0 && p2 >= 0 && manhattan(p1, p2, to_x, to_y) == 1) {
            cx = p1;
            cy = p2;
            have_cur = true;
        } else if (read_player_grid_for_adjacent_step(to_x, to_y, &cx, &cy)) {
            have_cur = true;
        }

        if (have_cur) {
            boost_adjacent_destination(cx, cy, &nx, &ny);
            if (nx != to_x || ny != to_y)
                FW_LOGD("boots9110 adj (%d,%d)->(%d,%d) boosted (%d,%d)", cx, cy, to_x, to_y, nx, ny);
        }
    }

    return g_orig_on_enter_grid(scene, p1, p2, nx, ny, flag);
}

} // namespace

void mod_fast_walk_install(uintptr_t cocos_elf_bias) {
    g_cocos_bias = cocos_elf_bias;

    g_fn_get_prop_count = reinterpret_cast<JhData_getPropCount_t>(cocos_elf_bias + kJhData_getPropCount);

    void *target = reinterpret_cast<void *>(cocos_elf_bias + kMainScene2_onEnterGrid);
    const int rc = DobbyHook(
            target,
            reinterpret_cast<dobby_dummy_func_t>(hooked_on_enter_grid),
            reinterpret_cast<dobby_dummy_func_t *>(&g_orig_on_enter_grid));

    FW_LOGD(
            "install onEnterGrid rc=%d hook=%p orig=%p getPropCount=%p (no cross-so std::string)",
            rc,
            target,
            reinterpret_cast<void *>(g_orig_on_enter_grid),
            reinterpret_cast<void *>(g_fn_get_prop_count));
}
