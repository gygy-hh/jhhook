#include "crash_trace.hpp"

#include <android/log.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <exception>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif

namespace {

constexpr char kTag[] = "CrashTrace";

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};

/// sigaction 槽位按信号编号索引（通常远小于 NSIG）
constexpr size_t kSigSlots = 128;

struct SigSlot {
    struct sigaction prev {};
    bool installed{};
};

SigSlot g_slots[kSigSlots];

std::atomic<bool> g_in_fatal_handler{false};

static long current_kernel_tid() {
#if defined(__ANDROID__) && defined(__NR_gettid)
    return static_cast<long>(syscall(__NR_gettid));
#else
    return static_cast<long>(getpid());
#endif
}

static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "?";
    }
}

/// 信号处理里尽量避免 malloc：小型格式化缓冲区
static void log_fatal_line(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    (void) vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    __android_log_print(ANDROID_LOG_FATAL, kTag, "%s", buf);
}

struct UnwindBuf {
    void **frames{};
    size_t max{};
    size_t count{};
};

static _Unwind_Reason_Code unwind_step(struct _Unwind_Context *ctx, void *arg) {
    auto *ub = static_cast<UnwindBuf *>(arg);
    if (ub->count >= ub->max)
        return _URC_END_OF_STACK;
    uintptr_t ip = _Unwind_GetIP(ctx);
    if (ip == 0)
        return _URC_NO_REASON;
    ub->frames[ub->count++] = reinterpret_cast<void *>(ip);
    return _URC_NO_REASON;
}

static size_t unwind_collect(void **frames, size_t max_frames) {
    UnwindBuf ub{frames, max_frames, 0};
    _Unwind_Backtrace(unwind_step, &ub);
    return ub.count;
}

static void log_one_frame(size_t idx, void *pc) {
    Dl_info inf{};
    if (dladdr(pc, &inf)) {
        const char *sym = inf.dli_sname ? inf.dli_sname : "?";
        char *demangled = nullptr;
        int status = 0;
        if (inf.dli_sname != nullptr && inf.dli_sname[0] != '\0') {
            demangled = abi::__cxa_demangle(inf.dli_sname, nullptr, nullptr, &status);
        }
        const char *show = (demangled && status == 0) ? demangled : sym;
        ptrdiff_t rel = inf.dli_fbase ? (reinterpret_cast<char *>(pc) - reinterpret_cast<char *>(inf.dli_fbase)) : -1;
        log_fatal_line(
                "  #%02zu pc %p  %s  (%s+0x%lx)",
                idx,
                pc,
                show,
                inf.dli_fname ? inf.dli_fname : "?",
                static_cast<unsigned long>(rel >= 0 ? static_cast<unsigned long>(rel) : 0ul));
        std::free(demangled);
    } else {
        log_fatal_line("  #%02zu pc %p  <unknown>", idx, pc);
    }
}

static void dump_backtrace(const char *reason) {
    void *frames[128]{};
    const size_t n = unwind_collect(frames, sizeof(frames) / sizeof(frames[0]));
    log_fatal_line("--- native stack (%s) frames=%zu pid=%ld tid=%ld pthread=%p ---",
            reason,
            n,
            static_cast<long>(getpid()),
            current_kernel_tid(),
            reinterpret_cast<void *>(pthread_self()));
    for (size_t i = 0; i < n; ++i)
        log_one_frame(i, frames[i]);
    log_fatal_line("--- end native stack ---");
}

static void crash_sigaction(int sig, siginfo_t *info, void *uctx) {
    (void) uctx;

    if (g_in_fatal_handler.exchange(true)) {
        signal(sig, SIG_DFL);
        raise(sig);
        _exit(128 + sig);
    }

    const int code = info ? info->si_code : -1;
    void *addr = info ? info->si_addr : nullptr;
    log_fatal_line(
            "FATAL signal=%d (%s) si_code=%d si_addr=%p pid=%ld tid=%ld uid=%ld",
            sig,
            signal_name(sig),
            code,
            addr,
            static_cast<long>(getpid()),
            current_kernel_tid(),
            static_cast<long>(getuid()));

    dump_backtrace("signal");

    /// 恢复默认处理再投递同信号，便于系统 tombstone / debugger 捕获
    struct sigaction def{};
    def.sa_handler = SIG_DFL;
    sigemptyset(&def.sa_mask);
    sigaction(sig, &def, nullptr);

    g_in_fatal_handler.store(false);
    raise(sig);

    _exit(128 + sig);
}

static void install_sigactions() {
    struct sigaction sa{};
    sa.sa_flags = static_cast<int>(SA_SIGINFO | SA_RESTART);
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = crash_sigaction;

    stack_t alt{};
    alt.ss_sp = std::malloc(SIGSTKSZ);
    if (alt.ss_sp != nullptr) {
        alt.ss_size = SIGSTKSZ;
        alt.ss_flags = 0;
        if (sigaltstack(&alt, nullptr) != 0) {
            std::free(alt.ss_sp);
            log_fatal_line("sigaltstack failed errno=%d (continuing without alt stack)", errno);
        }
        sa.sa_flags |= SA_ONSTACK;
    }

    for (int sig : kSignals) {
        if (sig <= 0 || static_cast<size_t>(sig) >= kSigSlots)
            continue;
        SigSlot &slot = g_slots[static_cast<size_t>(sig)];
        if (sigaction(sig, &sa, &slot.prev) != 0) {
            log_fatal_line("sigaction failed sig=%d errno=%d", sig, errno);
            continue;
        }
        slot.installed = true;
    }
}

[[noreturn]] static void terminate_logger() {
    log_fatal_line("std::terminate — uncaught C++ exception or noexcept violation");
    dump_backtrace("terminate");
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(134);
}

} // namespace

void crash_trace_install() {
    static std::atomic<bool> once{false};
    if (once.exchange(true))
        return;

    install_sigactions();
    std::set_terminate(terminate_logger);

    log_fatal_line(
            "crash_trace_install: handlers ready (SIGSEGV,SIGABRT,SIGBUS,SIGILL,SIGFPE + std::terminate)");
}
//蛇咬爹