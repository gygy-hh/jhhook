#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <errno.h>

#include "counter_hooks.hpp"
#include "crash_trace.hpp"
#include "dobby.h"
#include "mod_confusion.hpp"
#include "mod_fast_walk.hpp"

#define TAG "DobbyHook"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Hook 目标 RVA 与符号见 libcocos2dcpp.so.lst（`.text` EXPORT）及 libcocos2dcp.c 中同名函数 VA：
// - BattleMgr::selectAllPlayers / Node::getScaleX,setScaleX,getRotationY,setRotationY → game_hooks.cpp
// - JhPlayer::addHp(...,cocos2d::Vec2) 第 7 参为 Vec2 间接指针（AArch64 x6）→ counter_hooks.cpp
// - JhPlayer::getActShouJiPlayer(void)、onNewFightRound(int) → mod_confusion.cpp / game_hooks.cpp
// - SkillOneTime::onAttatckEvent(std::string const&)、beginAct(void) → counter_hooks.cpp
// - JhPlayer::onRoundBegin + CounterState（对齐 GaoyangMod::StateManager：还击标记 / 原技能 / 每对先手伤害）→ counter_state.cpp
// - Battle::onEndBattle → CounterState::clear_session；DrawScene 尾帧 FlushDerivative（libnative RVA 0x8E6888）未挂载（lst 在同 VA 为函数体内指令，且依赖外挂 Derivative 符号）。
// - MainScene2::onEnterGrid + JhData::getPropCount/getPlayerLocation → mod_fast_walk.cpp（背包道具 9110：一格改五格）。
// 换游戏版本后请用 .lst 重新核对 EXPORT 地址。
const char *TARGET_LIB_NAME = "libcocos2dcpp.so";

// -----------------------------------------------------------------------------
// 基址：必须与 libnative-lib.c 中 get_module_base("/proc/self/maps") 一致。
// Android 上 dlopen 返回值不可当作模块 load bias 使用。
// -----------------------------------------------------------------------------
static uintptr_t get_elf_load_bias_from_maps(const char *path_substring) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        LOGE("maps: fopen errno=%d", errno);
        return 0;
    }

    char line[768];
    uintptr_t min_start = UINTPTR_MAX;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, path_substring))
            continue;

        uintptr_t start = 0;
        uintptr_t end = 0;
        // start-end perms ...
        if (sscanf(line, "%zx-%zx", &start, &end) != 2)
            continue;
        if (start < min_start)
            min_start = start;
    }
    fclose(fp);

    if (min_start == UINTPTR_MAX)
        return 0;
    return min_start;
}

// 对齐 libnative：pthread 线程内先 sleep(5) 再 initialize_all_components（见 sub_96A30）。
static constexpr unsigned kInitialDelaySec = 5u;

static void *delay_hook_thread(void *) {
    LOGD("[THREAD] 启动；先延时 %u 秒（对齐 lib-native）…", kInitialDelaySec);
    sleep(kInitialDelaySec);

    int attempts = 0;
    while (attempts < 240) {
        uintptr_t base = get_elf_load_bias_from_maps(TARGET_LIB_NAME);
        if (base != 0) {
            LOGD("[THREAD] 检测到 %s 基址=%#zx（第 %d 次轮询）", TARGET_LIB_NAME, base, attempts + 1);
            dobby_enable_near_branch_trampoline();
            counter_hooks_install(base);
            mod_fast_walk_install(base);
            // ModConfusion：回合清理 / apply / getActShouJiPlayer 乱位 / BattleHead 朝向
            ModConfusion::install_confusion_hooks(base);
            break;
        }
        usleep(500 * 1000);
        attempts++;
    }

    if (attempts >= 240)
        LOGE("[THREAD] 超时：maps 中未找到 %s", TARGET_LIB_NAME);

    return nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_myapplication_MainActivity_stringFromJNI(JNIEnv *env, jobject) {
    return env->NewStringUTF(
            "libjhhook.so loaded; mod thread started in JNI_OnLoad (see logcat DobbyHook)");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_myapplication_MainActivity_getDobbyTestResult(JNIEnv *, jobject) {
    return 100;
}

extern "C" jint JNI_OnLoad(JavaVM *vm, void *) {
    crash_trace_install();

    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;

    pthread_t tid{};
    if (pthread_create(&tid, nullptr, delay_hook_thread, nullptr) != 0) {
        LOGE("pthread_create 失败 errno=%d", errno);
        return JNI_ERR;
    }
    pthread_detach(tid);

    return JNI_VERSION_1_6;
}
//蛇咬爹