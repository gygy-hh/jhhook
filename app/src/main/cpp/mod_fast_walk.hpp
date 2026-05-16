#pragma once

#include <cstdint>

/// 大地图格子移动：背包存在指定道具 ID 时，将单次 orthogonal 一格移动改为五格。
/// RVA 绑定当前仓库中的 libcocos2dcpp.so.lst（换包需重对导出）。
void mod_fast_walk_install(uintptr_t cocos_elf_bias);
