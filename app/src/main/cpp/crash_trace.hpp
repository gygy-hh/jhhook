#pragma once

/// 尽早安装 fatal 信号与 std::terminate 处理，向 logcat 输出进程/tid、信号详情与符号化解栈。
/// Tag: CrashTrace
void crash_trace_install();
//蛇咬爹