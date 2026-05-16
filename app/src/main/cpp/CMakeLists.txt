# 设置最低 CMake 版本
cmake_minimum_required(VERSION 3.22.1)

# 设置项目名称
project("MyApplication")

# 定义 Dobby 静态库的根目录
# ${CMAKE_SOURCE_DIR} 指向 app/src/main/cpp/
# ${CMAKE_ANDROID_ARCH_ABI} 会被替换为 arm64-v8a, armeabi-v7a
set(DOBBY_LIB_DIR ${CMAKE_SOURCE_DIR}/dobby-libs/${CMAKE_ANDROID_ARCH_ABI})


# 添加 Dobby 头文件路径
# 确保 dobby-include 文件夹是放在 cpp 目录下的
include_directories(
        ${CMAKE_SOURCE_DIR}/dobby-include
)


# 【关键修复】设置 16 KB 页面对齐链接器标志
# 解决了 Android 15+ 的兼容性警告和应用启动闪退问题
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")


# 产物：libjhhook.so（Java/Kotlin 侧 System.loadLibrary("jhhook")）
add_library(
        jhhook
        SHARED
        native-lib.cpp
        game_hooks.cpp
        mod_confusion.cpp
        counter_state.cpp
        counter_hooks.cpp
        mod_add_hp_extras.cpp
        mod_fast_walk.cpp
        crash_trace.cpp
)


# 链接 Dobby 静态库和 Android 系统库
target_link_libraries(
        jhhook

        # 链接 Dobby 静态库：libdobby.a 的完整路径
        ${DOBBY_LIB_DIR}/libdobby.a

        # 链接 Android 必需的系统库
        android
        log
        dl
)