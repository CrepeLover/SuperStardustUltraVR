// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string_view>
#include <cmath>
#include <atomic>
#include <bit>
#include <stdexcept>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <xbyak/xbyak.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#endif

#include <cstdio>
#include <thread>

#include "common/decoder.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/wait_stats.h"
#include "core/linker.h"
#include "core/vr/vr_runtime.h"
#include "core/memory.h"
#include "input/scripted_input.h"

namespace Libraries::Kernel::WaitStats {

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t NumFrames = 4;
constexpr size_t MaxScanWords = 8192; // 64 KiB of stack
constexpr size_t SitesShown = 40;

u32 IntervalSeconds() {
    static const u32 seconds = [] {
        const char* value = std::getenv("SHADPS4_LOG_WAITS");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return seconds;
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

struct Key {
    std::string thread;
    const char* what;
    std::array<VAddr, NumFrames> stack;

    bool operator==(const Key& other) const {
        return what == other.what && stack == other.stack && thread == other.thread;
    }
};

struct KeyHash {
    size_t operator()(const Key& key) const {
        size_t hash = std::hash<std::string>{}(key.thread) ^
                      (std::hash<const void*>{}(key.what) * 31);
        for (const VAddr address : key.stack) {
            hash = hash * 1000003 ^ std::hash<VAddr>{}(address);
        }
        return hash;
    }
};

struct Site {
    u64 calls{};
    u64 total_ns{};
    u64 max_ns{};
};

std::mutex g_mutex;
std::unordered_map<Key, Site, KeyHash> g_sites;
s64 g_window_start_ns = 0;
thread_local int t_depth = 0;

/// True when the bytes just before `address` (in a guest module) encode a call instruction, so
/// that `address` is a return address and not some other pointer into the module.
bool FollowsCall(VAddr address) {
    const auto* p = reinterpret_cast<const u8*>(address);
    const auto reg_field = [](u8 modrm) { return static_cast<u8>(modrm & 0xF8); };
    return p[-5] == 0xE8 ||                                                  // call rel32
           (p[-6] == 0xFF && p[-5] == 0x15) ||                               // call [rip+d32]
           (p[-2] == 0xFF && reg_field(p[-1]) == 0xD0) ||                    // call reg
           (p[-3] == 0x41 && p[-2] == 0xFF && reg_field(p[-1]) == 0xD0) ||   // call r8..r15
           (p[-2] == 0xFF && reg_field(p[-1]) == 0x10) ||                    // call [reg]
           (p[-3] == 0x41 && p[-2] == 0xFF && reg_field(p[-1]) == 0x10) ||   // call [r8..r15]
           (p[-3] == 0xFF && reg_field(p[-2]) == 0x50) ||                    // call [reg+d8]
           (p[-4] == 0x41 && p[-3] == 0xFF && reg_field(p[-2]) == 0x50) ||   // call [r8..+d8]
           (p[-6] == 0xFF && reg_field(p[-5]) == 0x90) ||                    // call [reg+d32]
           (p[-7] == 0x41 && p[-6] == 0xFF && reg_field(p[-5]) == 0x90);     // call [r8..+d32]
}

bool IsGuestReturn(const Core::Linker* linker, VAddr value) {
    const Core::Module* module = const_cast<Core::Linker*>(linker)->FindByAddress(value);
    return module != nullptr && value >= module->GetBaseAddress() + 8 && FollowsCall(value);
}

/// The title's return addresses on this thread's stack, nearest first. Guest code and the
/// emulator's calls share the stack the title gave the thread, so above the blocking call's frame
/// come its caller (the title, or an emulator wrapper that the title called) and then the title's
/// callers. The search starts at the slot holding the call's own return address, which skips
/// stale values in the frames below it.
std::array<VAddr, NumFrames> GuestStack(VAddr return_address) {
    std::array<VAddr, NumFrames> stack{};
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (linker == nullptr) {
        return stack;
    }
    const auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    // Guest threads run on the stacks the title gave them; the host thread's own stack is the
    // fallback (threads the emulator started).
    uintptr_t bottom = 0;
    uintptr_t top = 0;
    if (g_curthread != nullptr && g_curthread->attr.stackaddr_attr != nullptr) {
        bottom = reinterpret_cast<uintptr_t>(g_curthread->attr.stackaddr_attr);
        top = bottom + g_curthread->attr.stacksize_attr;
    }
    if (here < bottom || here >= top) {
#ifdef _WIN32
        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        bottom = low;
        top = high;
#else
        pthread_attr_t attr;
        void* stack_addr = nullptr;
        size_t stack_size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) != 0) {
            return stack;
        }
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        bottom = reinterpret_cast<uintptr_t>(stack_addr);
        top = bottom + stack_size;
#endif
    }
    if (here < bottom || here >= top) {
        return stack;
    }
    const auto* word = reinterpret_cast<const u64*>(here);
    const auto* end = reinterpret_cast<const u64*>(std::min<uintptr_t>(
        top, here + MaxScanWords * sizeof(u64)));
    while (word < end && *word != return_address) {
        ++word;
    }
    if (word == end) {
        return stack;
    }
    size_t found = 0;
    for (; word < end && found < NumFrames; ++word) {
        if (IsGuestReturn(linker, *word)) {
            stack[found++] = *word;
        }
    }
    return stack;
}

std::string FrameText(VAddr address) {
    if (address == 0) {
        return "-";
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    const Core::Module* module = linker != nullptr ? linker->FindByAddress(address) : nullptr;
    if (module == nullptr) {
        return fmt::format("{:#x}", address);
    }
    return fmt::format("{}+{:#x}", module->name, address - module->GetBaseAddress());
}

/// Logs and clears the window when it is due. Caller holds g_mutex.
void ReportIfDue(s64 now_ns) {
    const s64 interval_ns = static_cast<s64>(IntervalSeconds()) * 1'000'000'000;
    if (g_window_start_ns == 0) {
        g_window_start_ns = now_ns;
        return;
    }
    if (now_ns - g_window_start_ns < interval_ns) {
        return;
    }
    const double seconds = static_cast<double>(now_ns - g_window_start_ns) / 1e9;
    std::vector<std::pair<const Key*, const Site*>> list;
    list.reserve(g_sites.size());
    for (const auto& [key, site] : g_sites) {
        list.emplace_back(&key, &site);
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.second->total_ns > b.second->total_ns;
    });
    LOG_INFO(Lib_Kernel,
             "Waits in the last {:.1f} s, busiest {} of {} call sites (blocked ms per second, "
             "calls per second, average and longest ms, thread, call, title's stack):",
             seconds, std::min(list.size(), SitesShown), list.size());
    for (size_t i = 0; i < list.size() && i < SitesShown; ++i) {
        const Key& key = *list[i].first;
        const Site& site = *list[i].second;
        std::string stack;
        for (const VAddr address : key.stack) {
            if (address != 0) {
                stack += stack.empty() ? "" : " < ";
                stack += FrameText(address);
            }
        }
        LOG_INFO(Lib_Kernel, "  {:7.1f} ms/s {:7.1f}/s avg {:7.3f} max {:7.2f}  [{}] {} | {}",
                 static_cast<double>(site.total_ns) / 1e6 / seconds,
                 static_cast<double>(site.calls) / seconds,
                 static_cast<double>(site.total_ns) / 1e6 / static_cast<double>(site.calls),
                 static_cast<double>(site.max_ns) / 1e6, key.thread, key.what,
                 stack.empty() ? std::string("(no title frames found)") : stack);
    }
    g_sites.clear();
    g_window_start_ns = now_ns;
}

// SHADPS4_SAMPLE_MAIN=1: about a thousand times a second, note where the title's main thread is
// (the title's code, the emulator, Windows, a driver) and the title's return addresses on its
// stack, into main_samples.csv in the log folder. Shows what the main thread spends its time on
// between its waits.
#ifdef _WIN32
bool SampleMain() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_SAMPLE_MAIN");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

std::string HostModuleText(u64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return fmt::format("{:#x}", address);
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring wide{path};
    const size_t slash = wide.find_last_of(L"\\/");
    std::string name;
    for (const wchar_t c : wide.substr(slash == std::wstring::npos ? 0 : slash + 1)) {
        name += static_cast<char>(c < 128 ? c : '?');
    }
    return fmt::format("{}+{:#x}", name, address - reinterpret_cast<u64>(module));
}

/// Logs the main loop's update step and minimum frame time when they change, and with
/// SHADPS4_SSU_STEP_HZ=<hz> (a test setting) sets both to 1/hz.
void NoteLoopTiming(u64 loop_this) {
    static const float wanted = [] {
        const char* value = std::getenv("SHADPS4_SSU_STEP_HZ");
        const float hz = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
        return hz >= 30.0f && hz <= 240.0f ? 1.0f / hz : 0.0f;
    }();
    static float logged_step = -1.0f;
    static float logged_min = -1.0f;
    if (loop_this < 0x10000) {
        return;
    }
    const u64 object = *reinterpret_cast<const u64*>(loop_this + 8);
    if (object < 0x10000) {
        return;
    }
    auto* step = reinterpret_cast<float*>(object + 0xc);
    auto* min_frame = reinterpret_cast<float*>(object + 0x14);
    const auto plausible = [](float seconds) { return seconds > 0.001f && seconds < 0.2f; };
    if (!plausible(*step) || !plausible(*min_frame)) {
        return;
    }
    if (*step != logged_step || *min_frame != logged_min) {
        LOG_INFO(Lib_Kernel,
                 "Main loop at {:#x}: update step {:.6f} s ({:.2f} Hz), minimum frame time "
                 "{:.6f} s ({:.2f} Hz){}",
                 object, *step, 1.0f / *step, *min_frame, 1.0f / *min_frame,
                 wanted != 0.0f ? fmt::format("; setting both to {:.6f} s", wanted) : "");
    }
    if (wanted != 0.0f) {
        *step = wanted;
        *min_frame = wanted;
    }
    logged_step = *step;
    logged_min = *min_frame;
}

void RunMainSampler(HANDLE thread, uintptr_t bottom, uintptr_t top) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "main_samples.csv";
    std::FILE* out = _wfopen(path.c_str(), L"w");
    if (out == nullptr) {
        LOG_ERROR(Lib_Kernel, "Cannot write {}", path.string());
        return;
    }
    LOG_INFO(Lib_Kernel, "Sampling the main thread into {}", path.string());
    std::fputs("ms,where,frame1,frame2,frame3,frame4\n", out);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    const auto start = Clock::now();
    u64 written = 0;
    while (true) {
        LARGE_INTEGER due{};
        due.QuadPart = -10'000; // 1 ms, in 100 ns units, relative
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        // Nothing that allocates or locks between suspending and resuming: the main thread may
        // hold the heap or loader lock.
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            break;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        const bool have_context = GetThreadContext(thread, &context) != FALSE;
        std::array<VAddr, NumFrames> frames{};
        const u64* first_slot = nullptr;
        if (have_context && context.Rsp >= bottom && context.Rsp < top) {
            const auto* word = reinterpret_cast<const u64*>(context.Rsp);
            const auto* end = reinterpret_cast<const u64*>(
                std::min<uintptr_t>(top, context.Rsp + MaxScanWords * sizeof(u64)));
            size_t found = 0;
            for (; word < end && found < NumFrames; ++word) {
                if (IsGuestReturn(linker, *word)) {
                    if (found == 0) {
                        first_slot = word;
                    }
                    frames[found++] = *word;
                }
            }
        }
        // Super Stardust Ultra VR's main loop (eboot sub_426e10) yields at 0x426fed until its
        // minimum frame time has passed. Caught there, its frame gives the loop object: `this` at
        // the return slot + 0x18; [this + 8] holds the update step (+0xc) and the minimum frame
        // time (+0x14), in seconds.
        u64 loop_this = 0;
        if (first_slot != nullptr && frames[0] != 0 &&
            (context.Rip < frames[0] - 0x600000 || context.Rip > frames[0] + 0x600000)) {
            const Core::Module* frame_module = linker->FindByAddress(frames[0]);
            if (frame_module != nullptr && frame_module->name == "eboot.bin" &&
                frames[0] - frame_module->GetBaseAddress() == 0x426ff0) {
                loop_this = first_slot[3];
            }
        }
        ResumeThread(thread);
        if (!have_context) {
            continue;
        }
        if (loop_this != 0) {
            NoteLoopTiming(loop_this);
        }
        const Core::Module* module = linker->FindByAddress(context.Rip);
        const std::string where =
            module != nullptr
                ? fmt::format("{}+{:#x}", module->name, context.Rip - module->GetBaseAddress())
                : HostModuleText(context.Rip);
        fmt::print(out, "{:.2f},{},{},{},{},{}\n",
                   std::chrono::duration<double, std::milli>(Clock::now() - start).count(), where,
                   FrameText(frames[0]), FrameText(frames[1]), FrameText(frames[2]),
                   FrameText(frames[3]));
        if (++written % 1000 == 0) {
            std::fflush(out);
        }
    }
    std::fclose(out);
}

void MaybeStartMainSampler() {
    static std::once_flag started;
    if (!SampleMain() || g_curthread == nullptr || g_curthread->name != "Game:Main" ||
        g_curthread->attr.stackaddr_attr == nullptr) {
        return;
    }
    std::call_once(started, [] {
        HANDLE thread = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &thread,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_LIMITED_INFORMATION,
                             FALSE, 0)) {
            return;
        }
        const auto bottom = reinterpret_cast<uintptr_t>(g_curthread->attr.stackaddr_attr);
        const auto top = bottom + g_curthread->attr.stacksize_attr;
        std::thread{[thread, bottom, top] { RunMainSampler(thread, bottom, top); }}.detach();
    });
}
#else
bool SampleMain() {
    return false;
}
void MaybeStartMainSampler() {}
#endif

} // namespace

bool Enabled() {
    return IntervalSeconds() != 0 || SampleMain();
}

Scope::Scope(const char* what_, void* return_address_)
    : what{what_}, return_address{return_address_} {
    if (!Enabled()) {
        return;
    }
    MaybeStartMainSampler();
    if (t_depth++ == 0 && IntervalSeconds() != 0) {
        start_ns = NowNs();
    }
}

Scope::~Scope() {
    if (!Enabled()) {
        return;
    }
    --t_depth;
    if (start_ns < 0) {
        return;
    }
    const s64 now_ns = NowNs();
    const u64 elapsed = static_cast<u64>(now_ns - start_ns);
    Key key{
        .thread = g_curthread != nullptr ? g_curthread->name : std::string("host"),
        .what = what,
        .stack = GuestStack(reinterpret_cast<VAddr>(return_address)),
    };
    std::scoped_lock lock{g_mutex};
    Site& site = g_sites[std::move(key)];
    ++site.calls;
    site.total_ns += elapsed;
    site.max_ns = std::max<u64>(site.max_ns, elapsed);
    ReportIfDue(now_ns);
}

static std::atomic<bool> g_ssu_frozen{false};

bool SsuFreezeWorld(bool on) {
    constexpr u64 TimerPointer = 0x9b6250;
    constexpr u64 JumpWord = 0x426f08; // aligned 8 bytes; the jump is bytes 1-6
    constexpr float FrozenStep = 1e-5f;
    static constexpr std::array<u8, 6> SkipIfSlow{0x0f, 0x85, 0x91, 0x00, 0x00, 0x00}; // jne +0x91
    static constexpr std::array<u8, 6> SkipAlways{0xe9, 0x92, 0x00, 0x00, 0x00, 0x90}; // jmp; nop
    static std::mutex freeze_mutex;
    bool frozen = g_ssu_frozen.load();
    static bool jump_patched = false;
    static float saved_step = 0.0f;
    std::scoped_lock lock{freeze_mutex};
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < TimerPointer + 8) {
        return false;
    }
    const u64 object = *reinterpret_cast<const u64*>(base + TimerPointer);
    if (object < 0x10000) {
        return false;
    }
    auto* step = reinterpret_cast<float*>(object + 0xc);
    if (on == frozen) {
        if (on) {
            *step = FrozenStep;
        }
        return true;
    }
    if (on) {
        if (*step <= 0.001f || *step >= 0.2f) {
            LOG_WARNING(Lib_Kernel, "Freeze: implausible update step {}, not freezing", *step);
            return false;
        }
        saved_step = *step;
    }
#ifdef _WIN32
    auto* word = reinterpret_cast<volatile LONG64*>(base + JumpWord);
    if (reinterpret_cast<uintptr_t>(word) % 8 == 0 && (on || jump_patched)) {
        LONG64 value = *word;
        std::array<u8, 8> bytes{};
        std::memcpy(bytes.data(), &value, 8);
        const auto& from = on ? SkipIfSlow : SkipAlways;
        const auto& to = on ? SkipAlways : SkipIfSlow;
        if (std::memcmp(bytes.data() + 1, from.data(), from.size()) == 0) {
            std::memcpy(bytes.data() + 1, to.data(), to.size());
            std::memcpy(&value, bytes.data(), 8);
            DWORD old = 0;
            if (VirtualProtect(const_cast<LONG64*>(word), 8, PAGE_EXECUTE_READWRITE, &old)) {
                InterlockedExchange64(word, value);
                VirtualProtect(const_cast<LONG64*>(word), 8, old, &old);
                FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(word), 8);
                jump_patched = on;
            } else {
                LOG_WARNING(Lib_Kernel, "Freeze: cannot unprotect the main loop ({})",
                            GetLastError());
            }
        } else {
            LOG_WARNING(Lib_Kernel, "Freeze: the main loop's jump is not as expected");
        }
    }
#endif
    *step = on ? FrozenStep : saved_step;
    g_ssu_frozen = on;
    LOG_INFO(Lib_Kernel, "Freeze: game world {} (update step {:.6f} s{})",
             on ? "stopped" : "running again", *step,
             on ? (jump_patched ? ", world updates skipped" : ", world updates still run") : "");
    return true;
}

namespace {
std::atomic<float*> g_particle_step{nullptr};
} // namespace

void SsuPatchParticleStep() {
    const auto set = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr && *v != '\0';
    };
    if (!(set("SHADPS4_SSU_DRAW_FPS") || set("SHADPS4_SSU_FPS")) ||
        Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
        return;
    }
    constexpr u64 Constant = 0x5b90f4;                    // 1/59.94 (0x3c88ab86)
    constexpr std::array<u64, 2> Loads{0x422e1a, 0x422e80}; // vmovss xmm0, [rip + Constant]
    constexpr u64 Slot = 0xcfdf00; // the data segment's zero padding (its memory ends 0xcfd1ac)
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < 0xd00000) {
        return;
    }
    const auto at = [base](u64 offset) { return reinterpret_cast<u8*>(base + offset); };
    const auto disp_to = [](u64 load, u64 target) {
        return static_cast<s32>(static_cast<s64>(target) - static_cast<s64>(load + 8));
    };
    bool ok = *reinterpret_cast<const u32*>(at(Constant)) == 0x3c88ab86 &&
              *reinterpret_cast<const u32*>(at(Slot)) == 0;
    for (const u64 load : Loads) {
        s32 disp = 0;
        std::memcpy(&disp, at(load + 4), 4);
        ok = ok && std::memcmp(at(load), "\xc5\xfa\x10\x05", 4) == 0 &&
             disp == disp_to(load, Constant);
    }
    if (!ok) {
        LOG_WARNING(Lib_Kernel, "Particle step: the game's code is not as expected, left alone");
        return;
    }
    std::memcpy(at(Slot), at(Constant), 4);
#ifdef _WIN32
    for (const u64 load : Loads) {
        DWORD old = 0;
        if (!VirtualProtect(at(load), 8, PAGE_EXECUTE_READWRITE, &old)) {
            LOG_WARNING(Lib_Kernel, "Particle step: cannot unprotect the code ({})",
                        GetLastError());
            return;
        }
        const s32 disp = disp_to(load, Slot);
        std::memcpy(at(load + 4), &disp, 4);
        VirtualProtect(at(load), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at(load), 8);
    }
#endif
    g_particle_step = reinterpret_cast<float*>(at(Slot));
    LOG_INFO(Lib_Kernel, "Particle step: the per-frame particle updates read eboot+{:#x} (now {:g} s)",
             Slot, *reinterpret_cast<const float*>(at(Slot)));
}

void SsuSetParticleStep(float step) {
    float* slot = g_particle_step.load();
    if (slot == nullptr || !(step > 0.0f && step < 0.1f) || *slot == step) {
        return;
    }
    static u32 logged = 0;
    if (logged < 20) {
        ++logged;
        LOG_INFO(Lib_Kernel, "Particle step: {:g} s a drawn frame (was {:g})", step, *slot);
    }
    *reinterpret_cast<volatile float*>(slot) = step;
}

namespace {
struct RateConstant {
    u64 load;     // instruction reading the constant (rip-relative)
    u64 constant; // rodata address it reads
    bool retained; // a kept fraction (x *= m: m^k), else a lerp fraction (1 - (1 - f)^k)
    const char* what;
    float original = 0;
};
// ssuvr-work\analysis\hardcoded_lerps.py and hardcoded_damping.py (2026-11-07), read by hand.
RateConstant g_rate_constants[] = {
    {0x292225, 0x5b6ae0, false, "centipede heading lerp"},
    {0x292b1a, 0x5b6ae0, false, "centipede heading lerp (2)"},
    {0x296c9d, 0x5b6bbc, false, "centipede_black heading lerp"},
    {0x310cea, 0x5b76d4, false, "sub_3103c0 follow lerp"},
    {0x356d26, 0x5b7dd0, false, "token heading lerp"},
    {0x35748a, 0x5b7ddc, false, "token sphere lerp"},
    {0x3dfb4e, 0x5b8b60, false, "versus marker lerp"},
    {0x509f9e, 0x5b82d0, false, "menu slide lerp"},
    // The ship's velocity each update (sub_31b6b0): *0.95 steering, *0.9 coasting. Its boost top
    // speed is where that damping meets the push, so it fell ~16% at 120 Hz without this.
    {0x31cd76, 0x5b78c8, true, "ship velocity damping (steering)"},
    {0x31cdbd, 0x5b78c4, true, "ship velocity damping (coasting)"},
    {0x3dfb7a, 0x5b8b68, true, "versus marker velocity damping"},
};
constexpr u64 RateConstantSlots = 0xcfdf10; // data-segment zero padding, after the particle step
std::atomic<bool> g_rate_constants_on{false};
} // namespace

void SsuPatchRateConstants() {
    const char* fps = std::getenv("SHADPS4_SSU_FPS");
    if (fps == nullptr || *fps == '\0' || Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
        return;
    }
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < 0xd00000) {
        return;
    }
    const auto at = [base](u64 offset) { return reinterpret_cast<u8*>(base + offset); };
    // Check every site first: all or nothing.
    std::vector<std::pair<u32, u32>> disp_at; // displacement offset, instruction length
    for (const auto& site : g_rate_constants) {
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status =
            Common::Decoder::Instance()->decodeInstruction(inst, operands, at(site.load), 15);
        bool ok = ZYAN_SUCCESS(status) && inst.raw.disp.size == 32;
        bool rip = false;
        for (u32 i = 0; ok && i < inst.operand_count; ++i) {
            rip = rip || (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                          operands[i].mem.base == ZYDIS_REGISTER_RIP);
        }
        s32 disp = 0;
        if (ok) {
            std::memcpy(&disp, at(site.load + inst.raw.disp.offset), 4);
        }
        if (!ok || !rip ||
            static_cast<s64>(site.load + inst.length) + disp != static_cast<s64>(site.constant)) {
            LOG_WARNING(Lib_Kernel, "Rate constants: eboot+{:#x} is not as expected, none patched",
                        site.load);
            return;
        }
        disp_at.emplace_back(inst.raw.disp.offset, inst.length);
    }
    for (u32 i = 0; i < std::size(g_rate_constants); ++i) {
        if (*reinterpret_cast<const u32*>(at(RateConstantSlots + 4 * i)) != 0) {
            LOG_WARNING(Lib_Kernel, "Rate constants: slot area not free, none patched");
            return;
        }
    }
#ifdef _WIN32
    for (u32 i = 0; i < std::size(g_rate_constants); ++i) {
        auto& site = g_rate_constants[i];
        const u64 slot = RateConstantSlots + 4 * i;
        site.original = *reinterpret_cast<const float*>(at(site.constant));
        *reinterpret_cast<float*>(at(slot)) = site.original;
        const auto [offset, length] = disp_at[i];
        DWORD old = 0;
        if (!VirtualProtect(at(site.load), 16, PAGE_EXECUTE_READWRITE, &old)) {
            return;
        }
        const s32 disp = static_cast<s32>(static_cast<s64>(slot) -
                                          static_cast<s64>(site.load + length));
        std::memcpy(at(site.load + offset), &disp, 4);
        VirtualProtect(at(site.load), 16, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at(site.load), 16);
    }
    g_rate_constants_on = true;
    LOG_INFO(Lib_Kernel, "Rate constants: {} code lerps read slots at eboot+{:#x}",
             std::size(g_rate_constants), RateConstantSlots);
#endif
}

// Gold Melter beam spacing (2026-11-07). The beam adds a segment 60 times a second at any rate and a
// segment lives a fixed time, but its update (sub_26c7a0, dt in xmm0) puts every segment one
// update's travel (xmm7 = bullet speed x dt) beyond its newer neighbour, so at 120 Hz the gaps
// shrink (step, step, two steps) and the beam is ~30% shorter. The placement block 0x26c8de..0x26c91a
// (store the direction; position = neighbour + direction x xmm7) is moved to a cave that places it
// at the 60 Hz gap (xmm7 x rate/59.94) and moves the segment only k = 59.94/rate of the way there,
// so a bend still travels down the beam at the 60 Hz pace. Both factors are 1 at 60 Hz.
constexpr u64 BeamBlock = 0x26c8de;
constexpr u64 BeamBlockEnd = 0x26c91a;
constexpr u64 BeamCave = 0x6e7d00;  // the executable segment's zero padding, above the call stubs
constexpr u64 BeamSlots = 0xcfdf80; // data-segment zero padding: gap scale, catch-up fraction
std::atomic<bool> g_beam_patch_on{false};

void SsuPatchBeamSpacing() {
    const char* fps = std::getenv("SHADPS4_SSU_FPS");
    if (fps == nullptr || *fps == '\0' || Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
        return;
    }
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < 0xd00000) {
        return;
    }
    const auto at = [base](u64 offset) { return reinterpret_cast<u8*>(base + offset); };
    static constexpr u8 expected[] = {
        0xc5, 0xfa, 0x11, 0x4b, 0x28, 0xc5, 0xfa, 0x11, 0x53, 0x2c, 0xc5, 0xfa, 0x11, 0x43, 0x30,
        0xc5, 0xf2, 0x59, 0xcf, 0xc5, 0xea, 0x59, 0xd7, 0xc5, 0xfa, 0x59, 0xc7, 0xc4, 0xc1, 0x72,
        0x58, 0x4f, 0x1c, 0xc4, 0xc1, 0x6a, 0x58, 0x57, 0x20, 0xc4, 0xc1, 0x7a, 0x58, 0x47, 0x24,
        0xc5, 0xfa, 0x11, 0x4b, 0x1c, 0xc5, 0xfa, 0x11, 0x53, 0x20, 0xc5, 0xfa, 0x11, 0x43, 0x24};
    static_assert(sizeof(expected) == BeamBlockEnd - BeamBlock);
    if (std::memcmp(at(BeamBlock), expected, sizeof(expected)) != 0) {
        LOG_WARNING(Lib_Kernel, "Beam spacing: eboot+{:#x} is not as expected, not patched", BeamBlock);
        return;
    }
    for (u64 i = 0; i < 0x100; ++i) {
        if (*at(BeamCave + i) != 0 || (i < 8 && *at(BeamSlots + i) != 0)) {
            LOG_WARNING(Lib_Kernel, "Beam spacing: cave or slots not free, not patched");
            return;
        }
    }
#ifdef _WIN32
    *reinterpret_cast<float*>(at(BeamSlots)) = 1.0f;
    *reinterpret_cast<float*>(at(BeamSlots + 4)) = 1.0f;
    DWORD old_cave = 0;
    DWORD old_block = 0;
    if (!VirtualProtect(at(BeamCave), 0x100, PAGE_EXECUTE_READWRITE, &old_cave)) {
        return;
    }
    if (!VirtualProtect(at(BeamBlock), sizeof(expected), PAGE_EXECUTE_READWRITE, &old_block)) {
        VirtualProtect(at(BeamCave), 0x100, old_cave, &old_cave);
        return;
    }
    try {
        using namespace Xbyak::util;
        Xbyak::CodeGenerator c(0x100, at(BeamCave));
        // The direction (xmm1, xmm2, xmm0) as before; xmm3..xmm5 are free here (written before any
        // read on every path from 0x26c91a).
        c.vmovss(c.dword[rbx + 0x28], xmm1);
        c.vmovss(c.dword[rbx + 0x2c], xmm2);
        c.vmovss(c.dword[rbx + 0x30], xmm0);
        c.vmulss(xmm3, xmm7, c.dword[rip + at(BeamSlots)]); // the 60 Hz gap
        c.vmovss(xmm5, c.dword[rip + at(BeamSlots + 4)]);   // how far to move there
        const std::array<std::pair<Xbyak::Xmm, u32>, 3> parts{
            {{xmm1, 0x1c}, {xmm2, 0x20}, {xmm0, 0x24}}};
        for (const auto& [v, offset] : parts) {
            c.vmulss(v, v, xmm3);
            c.vaddss(v, v, c.dword[r15 + offset]); // where the 60 Hz rule puts it
            c.vmovss(xmm4, c.dword[rbx + offset]);
            c.vsubss(v, v, xmm4);
            c.vmulss(v, v, xmm5);
            c.vaddss(v, v, xmm4);
            c.vmovss(c.dword[rbx + offset], v);
        }
        c.jmp(at(BeamBlockEnd), Xbyak::CodeGenerator::T_NEAR);
        c.ready();
        std::array<u8, sizeof(expected)> jump{};
        jump.fill(0x90);
        jump[0] = 0xe9;
        const s32 rel = static_cast<s32>(static_cast<s64>(BeamCave) - static_cast<s64>(BeamBlock + 5));
        std::memcpy(jump.data() + 1, &rel, 4);
        std::memcpy(at(BeamBlock), jump.data(), jump.size());
        g_beam_patch_on = true;
        LOG_INFO(Lib_Kernel, "Beam spacing: eboot+{:#x} placement moved to eboot+{:#x} ({} bytes)",
                 BeamBlock, BeamCave, c.getSize());
    } catch (const std::exception& e) {
        LOG_ERROR(Lib_Kernel, "Beam spacing: cannot patch: {}", e.what());
    }
    VirtualProtect(at(BeamBlock), sizeof(expected), old_block, &old_block);
    VirtualProtect(at(BeamCave), 0x100, old_cave, &old_cave);
    FlushInstructionCache(GetCurrentProcess(), at(BeamBlock), sizeof(expected));
    FlushInstructionCache(GetCurrentProcess(), at(BeamCave), 0x100);
#endif
}

void SsuFollowGameRate() {
    struct Wanted {
        bool on = false;
        bool headset = false;
        float fixed = 0.0f;
    };
    static const Wanted wanted = [] {
        Wanted w;
        const char* value = std::getenv("SHADPS4_SSU_FPS");
        if (value == nullptr || *value == '\0') {
            return w;
        }
        if (std::string_view{value} == "headset") {
            w.on = w.headset = true;
        } else {
            w.fixed = std::strtof(value, nullptr);
            w.on = w.fixed >= 30.0f && w.fixed <= 240.0f;
        }
        LOG_INFO(Lib_Kernel, "Game frame rate: {}", w.headset ? "follows the headset"
                                                    : w.on    ? fmt::format("{:g} Hz", w.fixed)
                                                              : "invalid setting, left alone");
        return w;
    }();
    if (!wanted.on || g_ssu_frozen.load()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    const auto now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::milliseconds{100};

    constexpr u64 TimerPointer = 0x9b6250;
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < TimerPointer + 8) {
        return;
    }
    const u64 object = *reinterpret_cast<const u64*>(base + TimerPointer);
    if (object < 0x10000) {
        return;
    }
    auto* timer = reinterpret_cast<float*>(object + 8); // rate, step, cap rate, minimum frame
    if (!(timer[0] >= 20.0f && timer[0] <= 400.0f)) {
        return; // not set up yet
    }

    float target = wanted.fixed;
    if (wanted.headset) {
        // The host display's measured refresh once there is one (it stays, even when the host
        // pauses), else the emulated headset's configured refresh.
        auto& runtime = Core::Vr::Runtime::Instance();
        const auto display = runtime.GetDisplayRefresh();
        target = display.sequence != 0 && display.rate > 30.0f ? display.rate
                                                               : runtime.HeadsetRefreshRate();
        for (const float common : {60.0f, 72.0f, 80.0f, 90.0f, 120.0f, 144.0f}) {
            if (std::abs(target - common) <= common * 0.015f) {
                target = common;
            }
        }
        target = std::clamp(std::round(target), 60.0f, 240.0f);
        // Only follow a new rate once it has held for a second.
        static float candidate = 0.0f;
        static Clock::time_point candidate_since{};
        if (target != candidate) {
            candidate = target;
            candidate_since = now;
        }
        static float applied = 0.0f;
        if (applied != 0.0f && target != applied && now - candidate_since < std::chrono::seconds{1}) {
            target = applied;
        }
        applied = target;
    }
    // The game's registered debug values it applies once per world update without the time step
    // (2026-11-07 inventory: ssuvr-work\analysis\tunables_all.py, tunable_patterns.py,
    // tunable_table.py --cpp): rescaled so they act as much per second as at its own 59.94 Hz.
    // Moved: a lerp fraction f (x += (to - x) f) -> 1 - (1 - f)^k; Retained: a kept fraction m
    // (x = to + (x - to) m) -> m^k; Count: frames -> n / k; Linear: an increment a frame -> a k;
    // k = 59.94 / rate. Whenever a value is not the one written here, the game (or its Lua) set it:
    // that becomes the original to scale. SHADPS4_SSU_RESCALE=0 turns this off (for comparisons).
    enum class Rescale { Moved, Retained, Count, Linear };
    struct FrameTunable {
        u64 address;
        Rescale kind;
        const char* name;
        float original = 0;
        float applied = 0;
        float applied_rate = 0;
    };
    static FrameTunable tunables[] = {
        {0x9b2ebc, Rescale::Moved, "COMMAND_GAME__SHIP__UI__SHIELD__PARAMS__Lerp_Speed"},
        {0x9b33d4, Rescale::Moved, "COMMAND_GAME__SHIP__GATLING__SIGHTS_LERP_SPEED"},
        {0x9b4dbc, Rescale::Moved, "COMMAND_GAME__CARRIER__MOVEMENT__roll_lerp"},
        {0x9b4f6c, Rescale::Moved, "game__hud__morpheus_strike_meter__fall_lerp_factor"},
        {0x9b6c74, Rescale::Moved, "game__bullets__beam__audio__turn_vol_down_lerp"},
        {0x9b6cd4, Rescale::Moved, "game__bullets__beam__audio__hit_fade_lerp"},
        {0x9b6d14, Rescale::Moved, "game__bullets__beam__follow_previous_lerp"},
        {0x9b6df4, Rescale::Moved, "game__bullets__beam__collision_distance_to_turn_lerp__range_to_min"},
        {0x9b6ed4, Rescale::Moved, "game__bullets__beam__audio__turn_vol_up_lerp"},
        {0x9b79a4, Rescale::Moved, "game__enemies__boss1__turn_lerp"},
        {0x9b79c4, Rescale::Moved, "game__enemies__boss1__rot_apply_lerp"},
        {0x9b7aa4, Rescale::Moved, "game__enemies__boss1__approach_mul"},
        {0x9b7be4, Rescale::Moved, "game__enemies__boss1__speed_lerp"},
        {0x9b7e24, Rescale::Moved, "game__enemies__boss2__default_arm_angle_apply_lerp"},
        {0x9b7fc4, Rescale::Moved, "game__enemies__boss2__base_angle_lerp"},
        {0x9b80a4, Rescale::Moved, "game__enemies__boss2__approach_mul"},
        {0x9b8384, Rescale::Moved, "game__enemies__boss2missile__turn_lerp"},
        {0x9b88a4, Rescale::Moved, "game__enemies__boss5missile__turn_lerp"},
        {0x9b8984, Rescale::Moved, "game__enemies__boss5missileship__rot_speed_lerp"},
        {0x9b89c4, Rescale::Moved, "game__enemies__boss5missileship__approach_mul"},
        {0x9b8c84, Rescale::Moved, "game__enemies__cannon__turn_lerp"},
        {0x9b8ce4, Rescale::Retained, "game__enemies__cannon__movement_slowdown_mul"},
        {0x9b8d04, Rescale::Moved, "game__enemies__cannon__approach_mul"},
        {0x9b8da4, Rescale::Moved, "game__enemies__centipede__drop_down_lerp"},
        {0x9b8fa4, Rescale::Retained, "game__enemies__centipede__head_speed_slowdown"},
        {0x9b8fc4, Rescale::Moved, "game__enemies__centipede__head_turn_lerp"},
        {0x9b90a4, Rescale::Moved, "COMMAND_GAME__CREEPER__MOVEMENT__burrow_lerp"},
        {0x9b90e4, Rescale::Moved, "COMMAND_GAME__CREEPER__MOVEMENT__height_lerp"},
        {0x9b9144, Rescale::Moved, "game__enemies__centipede__head_collision_turn_lerp"},
        {0x9b91a4, Rescale::Moved, "game__enemies__centipede__head_scale_apply_lerp_factor"},
        {0x9b91c4, Rescale::Retained, "game__enemies__centipede__head_scale_per_frame_mul"},
        {0x9b92a4, Rescale::Moved, "game__enemies__centipede_black__drop_down_lerp"},
        {0x9b9444, Rescale::Retained, "game__enemies__centipede_black__head_speed_slowdown"},
        {0x9b9464, Rescale::Moved, "game__enemies__centipede_black__head_turn_lerp"},
        {0x9b94a4, Rescale::Moved, "game__enemies__centipede_black__head_collision_turn_lerp"},
        {0x9b9504, Rescale::Moved, "game__enemies__centipede_black__head_scale_apply_lerp_factor"},
        {0x9b9524, Rescale::Retained, "game__enemies__centipede_black__head_scale_per_frame_mul"},
        {0x9b96c4, Rescale::Retained, "game__enemies__lurker__speed_per_frame_mul"},
        {0x9b9704, Rescale::Retained, "game__enemies__lurker__speed_slowdown_mul"},
        {0x9b97a4, Rescale::Moved, "game__enemies__lurker__turn_lerp"},
        {0x9b97c4, Rescale::Moved, "game__enemies__lurker__shoot_aim_lerp"},
        {0x9b98a4, Rescale::Retained, "game__enemies__lurker__avoid_move_back_mul"},
        {0x9b9924, Rescale::Moved, "game__enemies__lurker__approach_mul"},
        {0x9b9ce4, Rescale::Moved, "game__enemies__mine__approach_mul"},
        {0x9b9f24, Rescale::Moved, "game__enemies__mine_black__approach_mul"},
        {0x9ba244, Rescale::Moved, "game__enemies__mine_green__approach_mul"},
        {0x9ba464, Rescale::Moved, "game__enemies__nuke__approach_mul"},
        {0x9ba624, Rescale::Retained, "game__enemies__spinner__speed_slowdown_mul"},
        {0x9ba6e4, Rescale::Moved, "game__enemies__spinner__turn_lerp_factor"},
        {0x9ba704, Rescale::Moved, "game__enemies__spinner__turn_lerp_factor_charge"},
        {0x9ba724, Rescale::Retained, "game__enemies__spinner__power_up_speed_mul"},
        {0x9ba7e4, Rescale::Moved, "game__enemies__spinner__rotation_speed_up_mul"},
        {0x9ba804, Rescale::Moved, "game__enemies__spinner__rotation_speed_down_mul"},
        {0x9ba8a4, Rescale::Moved, "game__enemies__spinner__approach_mul"},
        {0x9bab44, Rescale::Moved, "game__enemies__swarmer__principal_dir_lerp"},
        {0x9bab84, Rescale::Moved, "game__enemies__swarmer__speed_boost_up_lerp"},
        {0x9baba4, Rescale::Moved, "game__enemies__swarmer__speed_boost_down_lerp"},
        {0x9babc4, Rescale::Moved, "game__enemies__swarmer__approach_mul"},
        {0x9bacc4, Rescale::Moved, "game__enemies__vortex__approach_mul"},
        {0x9bad64, Rescale::Moved, "game__enemies__weaponship__turn_speed__lerp_min"},
        {0x9bad84, Rescale::Moved, "game__enemies__weaponship__turn_speed__lerp_max"},
        {0x9bada4, Rescale::Moved, "game__enemies__weaponship__turn_speed_change_lerp"},
        {0x9bade4, Rescale::Moved, "game__enemies__weaponship__approach_mul"},
        {0x9bafa4, Rescale::Retained, "game__enemies__weaponship__pod__wobble_mul"},
        {0x9bb72c, Rescale::Moved, "grid__hide_lerp_factor"},
        {0x9bb74c, Rescale::Moved, "grid__hide_fast_lerp_factor"},
        {0x9bb76c, Rescale::Moved, "grid__show_lerp_factor"},
        {0x9bbc8c, Rescale::Moved, "game__token__token_repulsion_fix_mul"},
        {0x9bbf34, Rescale::Moved, "game__phase_done_screen__game_slowdown_lerp"},
        {0x9bbf54, Rescale::Moved, "game__phase_done_screen__game_speedup_lerp"},
        {0x9bdcf4, Rescale::Retained, "game__plr_ship__controls__motion_sensor_filter_lerp"},
        {0x9bdf0c, Rescale::Count, "game__hud__multiplier_countdown_frames"},
        {0x9be27c, Rescale::Moved, "game__hud__boost_meter__fall_lerp_factor"},
        {0x9be7a4, Rescale::Moved, "game__plr_ship__boost_time_left_to_turn_lerp_factor__range_to_min"},
        {0x9be7c4, Rescale::Moved, "game__plr_ship__boost_time_left_to_turn_lerp_factor__range_to_max"},
        {0x9be824, Rescale::Moved, "game__plr_ship__boost_time_left_to_turn_lerp_factor_impact__range_to_min"},
        {0x9be844, Rescale::Moved, "game__plr_ship__boost_time_left_to_turn_lerp_factor_impact__range_to_max"},
        {0x9be994, Rescale::Retained, "game__plr_ship__tilt__vel_dampening_mul"},
        {0x9beab4, Rescale::Moved, "game__plr_ship__sound_thruster_silence_lerp"},
        {0x9bed9c, Rescale::Moved, "game__plr_ship__boost__speed_up_lerp"},
        {0x9bedbc, Rescale::Moved, "game__plr_ship__boost__speed_up_lerp_impact"},
        {0x9beddc, Rescale::Moved, "game__plr_ship__boost__speed_down_lerp"},
        {0x9bedfc, Rescale::Moved, "game__plr_ship__boost__speed_down_lerp_impact"},
        {0x9c717c, Rescale::Retained, "game__rock__rock__velocity_slowdown_multiplier"},
        {0x9c71dc, Rescale::Moved, "game__rock__rock__volume_approach_mul_map__range_to_min"},
        {0x9c71fc, Rescale::Moved, "game__rock__rock__volume_approach_mul_map__range_to_max"},
        {0x9c731c, Rescale::Retained, "game__rock__rock__rotation_slowdown_multiplier"},
        {0x9c7edc, Rescale::Retained, "game__rock__ice__velocity_slowdown_multiplier"},
        {0x9c7f3c, Rescale::Moved, "game__rock__ice__volume_approach_mul_map__range_to_min"},
        {0x9c7f5c, Rescale::Moved, "game__rock__ice__volume_approach_mul_map__range_to_max"},
        {0x9c807c, Rescale::Retained, "game__rock__ice__rotation_slowdown_multiplier"},
        {0x9c8c3c, Rescale::Retained, "game__rock__gold__velocity_slowdown_multiplier"},
        {0x9c8c9c, Rescale::Moved, "game__rock__gold__volume_approach_mul_map__range_to_min"},
        {0x9c8cbc, Rescale::Moved, "game__rock__gold__volume_approach_mul_map__range_to_max"},
        {0x9c8ddc, Rescale::Retained, "game__rock__gold__rotation_slowdown_multiplier"},
        {0x9c999c, Rescale::Retained, "game__rock__steel__velocity_slowdown_multiplier"},
        {0x9c99fc, Rescale::Moved, "game__rock__steel__volume_approach_mul_map__range_to_min"},
        {0x9c9a1c, Rescale::Moved, "game__rock__steel__volume_approach_mul_map__range_to_max"},
        {0x9ca6fc, Rescale::Retained, "game__rock__special_static__velocity_slowdown_multiplier"},
        {0x9ca75c, Rescale::Moved, "game__rock__special_static__volume_approach_mul_map__range_to_min"},
        {0x9ca77c, Rescale::Moved, "game__rock__special_static__volume_approach_mul_map__range_to_max"},
        {0x9ca89c, Rescale::Retained, "game__rock__special_static__rotation_slowdown_multiplier"},
        {0x9cb45c, Rescale::Retained, "game__rock__special_follower__velocity_slowdown_multiplier"},
        {0x9cb4bc, Rescale::Moved, "game__rock__special_follower__volume_approach_mul_map__range_to_min"},
        {0x9cb4dc, Rescale::Moved, "game__rock__special_follower__volume_approach_mul_map__range_to_max"},
        {0x9cb5fc, Rescale::Retained, "game__rock__special_follower__rotation_slowdown_multiplier"},
        {0x9cc2dc, Rescale::Retained, "game__rumble__explosion__big_motor_fade_mul"},
        {0x9cc2fc, Rescale::Retained, "game__rumble__explosion__small_motor_fade_mul"},
        {0x9cc39c, Rescale::Moved, "game__partial_screen_fade__end_fade_lerp_factor"},
        {0x9cc3bc, Rescale::Moved, "game__partial_screen_fade__lerp_factor"},
        {0x9cc58c, Rescale::Moved, "game__camera__move_lerp"},
        {0x9cc5ac, Rescale::Moved, "game__camera__move_lerp_far"},
        {0x9cc5cc, Rescale::Moved, "game__camera__move_lerp_coop"},
        {0x9cc5ec, Rescale::Moved, "game__camera__move_lerp_coop_far"},
        {0x9cc6cc, Rescale::Moved, "game__camera__setting_change_lerp"},
        {0x9ccb3c, Rescale::Moved, "COMMAND_GAME__TOKENS__MOVEMENT__Hover_lerp"},
        {0x9ccbbc, Rescale::Retained, "game__token__fly_end_slowdown_mul"},
        {0x9ccbdc, Rescale::Retained, "game__token__fly_slowdown_mul"},
        {0x9ccc7c, Rescale::Moved, "game__token__sphere_snap_mul"},
        {0x9ccf5c, Rescale::Moved, "menu__flyby_advert__fade_out_lerp"},
        {0x9cd1ec, Rescale::Moved, "menu__loading_object_show_lerp"},
        {0x9cd7ec, Rescale::Retained, "menu__background_fireworks__velocity_mul"},
        {0x9ce3b4, Rescale::Moved, "menu__startup_fader__fade_lerp"},
        {0x9ce8ac, Rescale::Retained, "press_start__logo_iir_first_enter_decay"},
        {0x9ce8cc, Rescale::Retained, "press_start__logo_iir_enter_decay"},
        {0x9ce90c, Rescale::Retained, "press_start__logo_iir_exit_decay"},
        {0x9cec1c, Rescale::Linear, "menu__terms_scroller__eula_scroll_speed_lines_per_frame"},
        {0x9cec3c, Rescale::Linear, "menu__terms_scroller__ingame_terms_scroll_speed_lines_per_frame"},
        {0x9cf06c, Rescale::Count, "cursor_blink_frames"},
        {0x9cf0ac, Rescale::Retained, "menu__movement_decay"},
        {0x9cf0cc, Rescale::Retained, "menu__button_up_iir_decay"},
        {0x9cf0ec, Rescale::Retained, "menu__button_down_iir_decay"},
        {0x9d6c94, Rescale::Retained, "game_versus__bullets__missile__friction"},
        {0x9d6d94, Rescale::Retained, "game_versus__bullets__mine__friction"},
        {0x9d73a4, Rescale::Retained, "game_versus__plr_physics__wall_hit_friction"},
        {0x9d75c4, Rescale::Moved, "game_versus__token_physics__map_repulsion_fix_mul"},
        {0x9d8cf4, Rescale::Moved, "game_versus__token__token_repulsion_fix_mul"},
        {0x9d91e4, Rescale::Retained, "game_versus__plr_ship__controls__motion_sensor_filter_lerp"},
        {0x9dc314, Rescale::Moved, "game_versus__plr_ship__turn_lerp_factor"},
        {0x9dc3d4, Rescale::Retained, "game_versus__plr_ship__engine_normal_friction"},
        {0x9dc434, Rescale::Retained, "game_versus__plr_ship__tilt__vel_dampening_mul"},
        {0x9dc4f4, Rescale::Moved, "game_versus__plr_ship__sound_thruster_silence_lerp"},
        {0x9dc904, Rescale::Retained, "game_versus__plr_ship__engine_exceed_max_speed_friction"},
        {0x9dd30c, Rescale::Retained, "game_versus__rumble__explosion__big_motor_fade_mul"},
        {0x9dd32c, Rescale::Retained, "game_versus__rumble__explosion__small_motor_fade_mul"},
        {0x9ddadc, Rescale::Moved, "game_versus__token__sphere_snap_mul"},
        {0x9ddbbc, Rescale::Retained, "game_versus__token__fly_slowdown_mul"},
        {0x9ddbdc, Rescale::Retained, "game_versus__token__fly_end_slowdown_mul"},
        // 2026-11-08, analysis\tunable_increments.py (increments and far store-backs), read by hand:
        // Invasion Zone carrier bomb, per update dir.xz *= 0.99, dir.y -= 0.005 (sub_26f920 0x2708ef);
        // swarmer flocking, per update velocity += (centre - pos) 0.04 + (mean v - v) 0.05 - avoid
        // 0.2 / 0.6, then normalised to the move speed (sub_2b4ad0 0x2b5a35..0x2b5c17); Invasion Zone
        // gatling barrel spin, per update x += (to - x) / Spin_Up_Time 0.05 (sub_213c10 0x214579,
        // Linear: within 3% of the exact lerp at 120 Hz).
        {0x9b707c, Rescale::Retained, "COMMAND_GAME__CARRIER__COMBAT__BULLET__BOMB__FALLOFF__HORIZONTAL"},
        {0x9b709c, Rescale::Linear, "COMMAND_GAME__CARRIER__COMBAT__BULLET__BOMB__FALLOFF__VERTICAL"},
        {0x9baa64, Rescale::Linear, "game__enemies__swarmer__boid_move_center_weight"},
        {0x9baa84, Rescale::Moved, "game__enemies__swarmer__boid_match_velocity_weight"},
        {0x9baaa4, Rescale::Linear, "game__enemies__swarmer__boid_avoid_close_weight"},
        {0x9baac4, Rescale::Linear, "game__enemies__swarmer__boid_avoid_close_bullet_weight"},
        {0x9b3474, Rescale::Linear, "COMMAND_GAME__SHIP__GATLING__BARRELS__ACCEL_SPEED"},
    };
    static const bool rescale_on = [] {
        const char* v = std::getenv("SHADPS4_SSU_RESCALE");
        return v == nullptr || *v != '0';
    }();
    constexpr float GameRate = 59.94f; // the only rate the game sets (HMD config loader)
    static u32 tunable_logs = 0;
    u32 rescaled = 0;
    if (rescale_on && MemoryPatcher::g_eboot_image_size > 0x9e0000) {
        const float k = GameRate / target;
        for (auto& tunable : tunables) {
            void* where = reinterpret_cast<void*>(base + tunable.address);
            const bool is_count = tunable.kind == Rescale::Count;
            const float current = is_count ? static_cast<float>(*static_cast<s32*>(where))
                                           : *static_cast<float*>(where);
            const bool ours = tunable.applied_rate != 0.0f && current == tunable.applied;
            if (ours && tunable.applied_rate == target) {
                continue;
            }
            if (!ours) {
                bool plausible = false;
                switch (tunable.kind) {
                case Rescale::Count:
                    plausible = current >= 1.0f && current <= 100000.0f;
                    break;
                case Rescale::Linear:
                    plausible = current > 0.0f && current < 1000.0f;
                    break;
                default:
                    plausible = current > 0.0f && current < 1.0f;
                    break;
                }
                if (!plausible) {
                    continue; // not set up yet, or not this game version's layout
                }
                tunable.original = current;
            }
            float value = tunable.original;
            switch (tunable.kind) {
            case Rescale::Moved:
                value = 1.0f - std::pow(1.0f - tunable.original, k);
                break;
            case Rescale::Retained:
                value = std::pow(tunable.original, k);
                break;
            case Rescale::Count:
                value = static_cast<float>(
                    std::max(1, static_cast<s32>(std::lround(tunable.original / k))));
                break;
            case Rescale::Linear:
                value = tunable.original * k;
                break;
            }
            if (is_count) {
                *static_cast<s32*>(where) = static_cast<s32>(value);
            } else {
                *static_cast<float*>(where) = value;
            }
            tunable.applied = value;
            tunable.applied_rate = target;
            ++rescaled;
            if (tunable_logs < 12) {
                ++tunable_logs;
                LOG_INFO(Lib_Kernel, "Game frame rate: {} {:g} -> {:.5g}", tunable.name,
                         tunable.original, value);
            }
        }
        if (rescaled > 0) {
            static u32 summaries = 0;
            if (summaries++ < 20) {
                LOG_INFO(Lib_Kernel, "Game frame rate: {} per-update values rescaled for {:g} Hz",
                         rescaled, target);
            }
        }
    }
    if (g_rate_constants_on.load()) {
        static float constants_rate = 0.0f;
        if (constants_rate != target) {
            constants_rate = target;
            const float k = GameRate / target;
            for (u32 i = 0; i < std::size(g_rate_constants); ++i) {
                const auto& site = g_rate_constants[i];
                const float v = site.retained ? std::pow(site.original, k)
                                              : 1.0f - std::pow(1.0f - site.original, k);
                *reinterpret_cast<float*>(base + RateConstantSlots + 4 * i) = v;
            }
            LOG_INFO(Lib_Kernel,
                     "Rate constants: {} code lerps / dampings rescaled for {:g} Hz (e.g. {:g} -> "
                     "{:.5g}, ship velocity {:g} -> {:.5g})",
                     std::size(g_rate_constants), target, g_rate_constants[0].original,
                     *reinterpret_cast<const float*>(base + RateConstantSlots),
                     g_rate_constants[8].original,
                     *reinterpret_cast<const float*>(base + RateConstantSlots + 4 * 8));
        }
    }
    if (g_beam_patch_on.load()) {
        static float beam_rate = 0.0f;
        if (beam_rate != target) {
            beam_rate = target;
            const float k = std::min(static_cast<float>(GameRate / target), 1.0f);
            *reinterpret_cast<float*>(base + BeamSlots) = 1.0f / k;
            *reinterpret_cast<float*>(base + BeamSlots + 4) = k;
            LOG_INFO(Lib_Kernel, "Beam spacing: gap x{:.4g}, catch-up {:.4g} for {:g} Hz", 1.0f / k,
                     k, target);
        }
    }
    const float step = 1.0f / target;
    SsuSetParticleStep(step); // one world update a frame: particles age by its step
    if (timer[0] == target && timer[1] == step && timer[2] == target && timer[3] == step) {
        return;
    }
    static u32 logged = 0;
    if (logged < 20) {
        ++logged;
        LOG_INFO(Lib_Kernel,
                 "Game frame rate: timer had rate {:.2f} Hz, step {:.6f} s, cap {:.2f} Hz, minimum "
                 "frame {:.6f} s; now {:g} Hz{}",
                 timer[0], timer[1], timer[2], timer[3], target,
                 wanted.headset ? " (the headset's refresh)" : "");
    }
    timer[0] = target;
    timer[1] = step;
    timer[2] = target;
    timer[3] = step;
}

void SsuPatchFrameLoop() {
    const char* value = std::getenv("SHADPS4_SSU_DRAW_FPS");
    if (value == nullptr || *value == '\0' ||
        Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
        return;
    }
    float target = std::string_view{value} == "headset"
                       ? Core::Vr::Runtime::Instance().HeadsetRefreshRate()
                       : std::strtof(value, nullptr);
    for (const float common : {60.0f, 72.0f, 80.0f, 90.0f, 120.0f, 144.0f}) {
        if (std::abs(target - common) <= common * 0.015f) {
            target = common;
        }
    }
    if (!(target >= 30.0f && target <= 480.0f)) {
        LOG_WARNING(Lib_Kernel, "Draw rate: SHADPS4_SSU_DRAW_FPS={} is not 30-480 Hz, left alone",
                    value);
        return;
    }
    // World updates per tick: the fraction nearest 59.94 / target with at most 12 ticks.
    constexpr double GameRate = 59.94;
    u32 updates = 1;
    u32 ticks = 1;
    double best = 1e9;
    for (u32 n = 1; n <= 12; ++n) {
        const u32 u = std::clamp<u32>(static_cast<u32>(std::lround(n * GameRate / target)), 1, n);
        const double error = std::abs(static_cast<double>(u) / n - GameRate / target);
        if (error < best - 1e-9) {
            best = error;
            updates = u;
            ticks = n;
        }
    }
    if (updates == ticks) {
        LOG_INFO(Lib_Kernel, "Draw rate: {:g} Hz is the game's own, nothing to do", target);
        return;
    }
    const u32 once_mask = [] {
        const char* v = std::getenv("SHADPS4_SSU_DRAW_ONCE");
        return v == nullptr || *v == '\0' ? 0u : static_cast<u32>(std::strtoul(v, nullptr, 0)) & 7;
    }();
    if (std::getenv("SHADPS4_SSU_FPS") != nullptr && *std::getenv("SHADPS4_SSU_FPS") != '\0') {
        LOG_WARNING(Lib_Kernel, "Draw rate: SHADPS4_SSU_FPS is set too; the world then runs at "
                                "that rate, the frames at {}/{} of its steps",
                    updates, ticks);
    }

    // eboot addresses. The loop keeps the step at [rbp-0x60] (from [timer+0xc] at 0x426ea7), the
    // minimum frame at [rbp-0x5c]; [rbp-0x70] is free in its frame.
    constexpr u64 InitAt = 0x426eb0;   // store step; load, store minimum frame (15 bytes)
    constexpr u64 GateAt = 0x426f05;   // test r12b,1; jne skip (10 bytes)
    constexpr u64 UpdateAt = 0x426f0f; // the world update
    constexpr u64 SkipAt = 0x426fa0;   // the slow-frame skip of an update
    constexpr u64 AfterAt = 0x426fa8;  // past the update, xmm0 = the mark's step
    constexpr std::array<u64, 3> StepLoads{0x426f1c, 0x426f37, 0x426f51}; // the update calls' step
    constexpr u64 CodeEnd = 0x6e4e68;  // end of the executable segment's contents
    constexpr u64 CaveAt = 0x6e7e00;   // its zero padding (mapped to 0x6e8000)
    constexpr u64 CaveSize = 0x200;
    constexpr u64 DataEnd = 0xcfd1ac;  // end of the data segment's memory
    constexpr u64 TickAt = 0xcfff00;   // its zero padding (mapped to 0xd00000)
    constexpr u64 PoseOnlyAt = TickAt + 4; // byte: the logic update returns after its pose block
    constexpr u64 FrameTimeAt = TickAt + 8; // float: frame time since the after-update calls ran
    constexpr u64 AfterCallsAt = 0x4270bc;  // mov rax,[rbp-0x78]; vmovaps xmm0,xmm2 (8 bytes)
    constexpr u64 AfterCallsNext = 0x4270c4;
    constexpr u64 AfterCallsEnd = 0x4270fb; // past the three after-update calls
    constexpr u64 PoseBlockAt = 0x26184a;  // Game_Logic_Impl::update's pose block ...
    constexpr u64 PoseBlockEnd = 0x261b14; // ... up to mov al, [0x9b6378] (6 bytes)
    constexpr u64 PoseBlockNext = 0x261b1a;
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < TickAt + 0x100) {
        LOG_WARNING(Lib_Kernel, "Draw rate: the game's executable is not as expected, left alone");
        return;
    }
    const auto at = [base](u64 offset) { return reinterpret_cast<u8*>(base + offset); };
    const auto is = [&](u64 offset, std::initializer_list<u8> bytes) {
        return std::memcmp(at(offset), std::data(bytes), bytes.size()) == 0;
    };
    const auto zero = [&](u64 offset, u64 size) {
        return std::all_of(at(offset), at(offset + size), [](u8 b) { return b == 0; });
    };
    static_assert(CaveAt >= CodeEnd && TickAt >= DataEnd);
    bool expected = is(0x426ea7, {0xc5, 0xfa, 0x10, 0x40, 0x0c}) &&
                    is(InitAt, {0xc5, 0xfa, 0x11, 0x45, 0xa0, 0xc5, 0xfa, 0x10, 0x40, 0x14, 0xc5,
                                0xfa, 0x11, 0x45, 0xa4}) &&
                    is(GateAt, {0x41, 0xf6, 0xc4, 0x01, 0x0f, 0x85, 0x91, 0x00, 0x00, 0x00}) &&
                    is(UpdateAt, {0xe8}) && is(SkipAt, {0xc5, 0xfa, 0x10, 0x45, 0xa0}) &&
                    is(AfterAt, {0x41, 0xf6, 0xc5, 0x01}) && zero(CaveAt, CaveSize) &&
                    zero(TickAt, 0x100) &&
                    is(0x261610, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                  0x54, 0x53, 0x48, 0x83, 0xe4, 0xe0, 0x48, 0x81, 0xec, 0xa0, 0x07,
                                  0x00, 0x00}) &&
                    is(PoseBlockAt, {0x48, 0x8b, 0x05, 0xdf, 0xcb, 0xa7, 0x00, 0x44, 0x8b, 0x2d}) &&
                    is(PoseBlockEnd, {0x8a, 0x05, 0x5e, 0x48, 0x75, 0x00, 0x24, 0x01}) &&
                    is(0x261f60, {0xe9}) && is(0x26196f, {0xe8}) &&
                    is(AfterCallsAt, {0x48, 0x8b, 0x45, 0x88, 0xc5, 0xf8, 0x28, 0xc2}) &&
                    is(AfterCallsNext, {0xc5, 0xfa, 0x11, 0x55, 0xa4}) &&
                    is(AfterCallsEnd, {0x8a, 0x05, 0x7f, 0x82, 0x8b, 0x00}) &&
                    is(0x426f8c, {0x41, 0xff, 0xc6}); // inc r14d: the updates this frame
    for (const u64 load : StepLoads) {
        expected = expected && is(load, {0xc5, 0xfa, 0x10, 0x45, 0xa0});
    }
    if (!expected) {
        LOG_WARNING(Lib_Kernel, "Draw rate: the game's main loop is not as expected, left alone");
        return;
    }

    std::string cave_text;
#ifdef _WIN32
    // Written before the executable runs; made writable for it, as the guest's own protection
    // may already be on the pages.
    DWORD old_loop = 0;
    DWORD old_cave = 0;
    DWORD old_pose = 0;
    if (!VirtualProtect(at(PoseBlockEnd), 8, PAGE_EXECUTE_READWRITE, &old_pose) ||
        !VirtualProtect(at(InitAt), 0x300, PAGE_EXECUTE_READWRITE, &old_loop) ||
        !VirtualProtect(at(CaveAt), CaveSize, PAGE_EXECUTE_READWRITE, &old_cave)) {
        LOG_WARNING(Lib_Kernel, "Draw rate: cannot unprotect the main loop ({})", GetLastError());
        return;
    }
#endif
    try {
        using namespace Xbyak::util;
        Xbyak::CodeGenerator c(CaveSize, at(CaveAt));
        Xbyak::Label ratio;
        Xbyak::Label no_update;
        const void* tick = at(TickAt);
        // Called from InitAt, xmm0 = the update step, rax = the frame timer.
        c.vmovss(c.dword[rbp - 0x70], xmm0); // the update calls' step
        c.vmulss(xmm0, xmm0, c.dword[rip + ratio]);
        c.vmovss(c.dword[rbp - 0x60], xmm0); // the mark's step: a tick
        c.vmovss(xmm0, c.dword[rax + 0x14]);
        c.vmulss(xmm0, xmm0, c.dword[rip + ratio]);
        c.vmovss(c.dword[rbp - 0x5c], xmm0); // the minimum frame: as many times less
        c.ret();
        c.align(16);
        // Jumped to from GateAt + 4, the flags of test r12b,1 still set; rax is free there.
        const u64 gate = static_cast<u64>(c.getCurr() - at(0));
        c.jne(at(SkipAt));
        c.mov(eax, c.dword[rip + tick]);
        c.add(eax, updates);
        c.cmp(eax, ticks);
        c.jb(no_update, Xbyak::CodeGenerator::T_NEAR);
        c.sub(eax, ticks);
        c.mov(c.dword[rip + tick], eax);
        c.jmp(at(UpdateAt), Xbyak::CodeGenerator::T_NEAR);
        Xbyak::Label pose_refresh;
        Xbyak::Label after_pose;
        c.L(no_update);
        c.mov(c.dword[rip + tick], eax);
        static const bool fresh_pose = [] {
            const char* v = std::getenv("SHADPS4_SSU_DRAW_POSE");
            return v == nullptr || std::string_view{v} != "0";
        }();
        c.test(r13b, 1); // the first tick of this frame: it is drawn after the loop
        c.jz(after_pose, Xbyak::CodeGenerator::T_NEAR);
        if (fresh_pose) {
            c.call(pose_refresh);
        }
        c.L(after_pose);
        c.vmovss(xmm0, c.dword[rbp - 0x60]);
        c.jmp(at(AfterAt), Xbyak::CodeGenerator::T_NEAR);
        c.align(16);
        // The pose block as a function: the update's own frame (rbp, callee-saved registers,
        // 32-aligned 0x7a0 bytes), into the block; PoseOnlyAt makes its end return. Only when the
        // update would get there itself: headset on (0xcde268 bit 0, 0xcde688), its handle
        // (0xcde2f0) valid, the tracker context's device (+8) registered.
        Xbyak::Label no_pose;
        c.L(pose_refresh);
        c.test(c.byte[rip + at(0xcde268)], 1);
        c.jz(no_pose, Xbyak::CodeGenerator::T_NEAR);
        c.cmp(c.byte[rip + at(0xcde688)], 0);
        c.je(no_pose, Xbyak::CodeGenerator::T_NEAR);
        c.cmp(c.dword[rip + at(0xcde2f0)], 0);
        c.jl(no_pose, Xbyak::CodeGenerator::T_NEAR);
        c.mov(rax, c.qword[rip + at(0xcde430)]);
        c.test(rax, rax);
        c.jz(no_pose, Xbyak::CodeGenerator::T_NEAR);
        c.cmp(c.dword[rax + 8], -1);
        c.je(no_pose, Xbyak::CodeGenerator::T_NEAR);
        c.mov(c.byte[rip + at(PoseOnlyAt)], 1);
        c.push(rbp);
        c.mov(rbp, rsp);
        c.push(r15);
        c.push(r14);
        c.push(r13);
        c.push(r12);
        c.push(rbx);
        c.and_(rsp, -32);
        c.sub(rsp, 0x7a0);
        c.jmp(at(PoseBlockAt), Xbyak::CodeGenerator::T_NEAR);
        c.L(no_pose);
        c.ret();
        c.align(16);
        // Jumped to from PoseBlockEnd: the update goes on, or the pose-only call returns.
        const u64 pose_end = static_cast<u64>(c.getCurr() - at(0));
        Xbyak::Label pose_only;
        c.cmp(c.byte[rip + at(PoseOnlyAt)], 0);
        c.jne(pose_only, Xbyak::CodeGenerator::T_NEAR);
        c.mov(al, c.byte[rip + at(0x9b6378)]);
        c.jmp(at(PoseBlockNext), Xbyak::CodeGenerator::T_NEAR);
        c.L(pose_only);
        c.mov(c.byte[rip + at(PoseOnlyAt)], 0);
        c.lea(rsp, c.ptr[rbp - 0x28]);
        c.pop(rbx);
        c.pop(r12);
        c.pop(r13);
        c.pop(r14);
        c.pop(r15);
        c.pop(rbp);
        c.ret();
        c.align(16);
        // Jumped to from AfterCallsAt, xmm2 = this frame's time (clamped), r14d = the world
        // updates it ran (callee-saved: still so after each call). The three after-update calls
        // made here: those in once_mask (SHADPS4_SSU_DRAW_ONCE, bit 0 [[rbp-0x78]]->vfunc8,
        // bit 1 [0x98a358]->vfunc10 = Game_Logic_Impl per frame, bit 2 [0x9b63a0]->vfunc60) only
        // in frames with a world update, given the time since they last ran ([rbp-0x70], free
        // after the loop); the others every frame with the frame's time ([rbp-0x5c], as the loop).
        const u64 after_calls = static_cast<u64>(c.getCurr() - at(0));
        {
            Xbyak::Label had_update;
            Xbyak::Label calls;
            c.vmovss(c.dword[rbp - 0x5c], xmm2);
            c.vaddss(xmm1, xmm2, c.dword[rip + at(FrameTimeAt)]);
            c.test(r14d, r14d);
            c.jnz(had_update, Xbyak::CodeGenerator::T_NEAR);
            c.vmovss(c.dword[rip + at(FrameTimeAt)], xmm1);
            c.jmp(calls, Xbyak::CodeGenerator::T_NEAR);
            c.L(had_update);
            c.mov(c.dword[rip + at(FrameTimeAt)], 0);
            c.vmovss(c.dword[rbp - 0x70], xmm1);
            c.L(calls);
            for (u32 k = 0; k < 3; ++k) {
                Xbyak::Label skip;
                const bool once = (once_mask >> k) & 1;
                if (once) {
                    c.test(r14d, r14d);
                    c.jz(skip, Xbyak::CodeGenerator::T_NEAR);
                }
                if (k == 0) {
                    c.mov(rax, c.qword[rbp - 0x78]);
                    c.mov(rdi, c.qword[rax]);
                } else {
                    c.mov(rdi, c.qword[rip + at(k == 1 ? 0x98a358 : 0x9b63a0)]);
                    c.test(rdi, rdi);
                    c.jz(skip, Xbyak::CodeGenerator::T_NEAR);
                }
                c.vmovss(xmm0, c.dword[rbp - (once ? 0x70 : 0x5c)]);
                c.mov(rax, c.qword[rdi]);
                c.call(c.ptr[rax + (k == 0 ? 0x8 : k == 1 ? 0x10 : 0x60)]); // ptr: Xbyak wants no size here
                c.L(skip);
            }
            c.jmp(at(AfterCallsEnd), Xbyak::CodeGenerator::T_NEAR);
        }
        c.align(4);
        c.L(ratio);
        c.dd(std::bit_cast<u32>(static_cast<float>(updates) / static_cast<float>(ticks)));
        c.ready();
        for (size_t i = 0; i < c.getSize(); ++i) {
            cave_text += fmt::format("{:02x}", c.getCode()[i]);
        }
        // Into the loop: call the init, jump to the gate, the update calls' step from [rbp-0x70].
        const auto rel = [&](u64 from_end, u64 to) {
            const s64 d = static_cast<s64>(to) - static_cast<s64>(from_end);
            const s32 d32 = static_cast<s32>(d);
            if (d32 != d) {
                throw std::runtime_error("the patch is out of a jump's reach");
            }
            return std::bit_cast<std::array<u8, 4>>(d32);
        };
        std::array<u8, 15> init{0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90,
                                0x90, 0x90, 0x90, 0x90, 0x90};
        const auto init_rel = rel(InitAt + 5, CaveAt);
        std::memcpy(init.data() + 1, init_rel.data(), 4);
        std::array<u8, 6> jump{0xe9, 0, 0, 0, 0, 0x90};
        const auto jump_rel = rel(GateAt + 4 + 5, gate);
        std::memcpy(jump.data() + 1, jump_rel.data(), 4);
        std::memcpy(at(InitAt), init.data(), init.size());
        std::memcpy(at(GateAt + 4), jump.data(), jump.size());
        std::array<u8, 6> pose_jump{0xe9, 0, 0, 0, 0, 0x90};
        const auto pose_rel = rel(PoseBlockEnd + 5, pose_end);
        std::memcpy(pose_jump.data() + 1, pose_rel.data(), 4);
        std::memcpy(at(PoseBlockEnd), pose_jump.data(), pose_jump.size());
        std::array<u8, 8> calls_jump{0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        const auto calls_rel = rel(AfterCallsAt + 5, after_calls);
        std::memcpy(calls_jump.data() + 1, calls_rel.data(), 4);
        std::memcpy(at(AfterCallsAt), calls_jump.data(), calls_jump.size());
        for (const u64 load : StepLoads) {
            *at(load + 4) = 0x90; // [rbp-0x60] -> [rbp-0x70]
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Lib_Kernel, "Draw rate: cannot write the loop patch: {}", e.what());
        cave_text.clear();
    }
#ifdef _WIN32
    VirtualProtect(at(PoseBlockEnd), 8, old_pose, &old_pose);
    FlushInstructionCache(GetCurrentProcess(), at(PoseBlockEnd), 8);
    VirtualProtect(at(InitAt), 0x300, old_loop, &old_loop);
    VirtualProtect(at(CaveAt), CaveSize, old_cave, &old_cave);
    FlushInstructionCache(GetCurrentProcess(), at(InitAt), 0x300);
    FlushInstructionCache(GetCurrentProcess(), at(CaveAt), CaveSize);
#endif
    if (cave_text.empty()) {
        return;
    }
    // Particles update once a drawn frame: each stands for its share of a world update.
    SsuSetParticleStep(*reinterpret_cast<const float*>(at(0x5b90f4)) * static_cast<float>(updates) /
                       static_cast<float>(ticks));
    LOG_INFO(Lib_Kernel,
             "Draw rate: {:g} Hz asked; frames at {:.2f} Hz ({} world update{} in {} ticks of "
             "1/{:.2f} s), world at the game's own 59.94 updates a second, a fresh head pose on "
             "every frame, after-update calls once an update: mask {}; loop patch at eboot+{:#x}: "
             "{}",
             target, GameRate * ticks / updates, updates, updates == 1 ? "" : "s", ticks,
             GameRate * ticks / updates, once_mask, CaveAt, cave_text);
}

namespace {
struct CallCounter {
    u64 function;     // eboot offset
    u64 data;         // eboot offset: u32 count, f32 last xmm0, u64 last return address, u64 last rdi
    u32 reported = 0; // count at the last report
};
std::vector<CallCounter> g_call_counters;

// SHADPS4_SSU_WATCH=<function>,<pointer offset>,<offset>[,<offset> x3] (hex): hardware write
// watchpoints (4 bytes each) on [rdi + pointer offset] + offset, rdi = the first argument of the
// last call of <function> (which must be counted too, SHADPS4_SSU_COUNT). Every writing
// instruction is tallied (the address after it, eboot-relative) and reported with the calls.
struct WatchHit {
    std::atomic<u64> rip{0};
    std::atomic<u32> count{0};
    std::atomic<u32> slot{0};
    std::atomic<u32> value{0};
};
struct Watch {
    u64 function = 0;
    u64 pointer_offset = 0;
    std::vector<u64> offsets;
    u64 rdi_data = 0;  // eboot offset of the function's saved rdi
    u64 watching = 0;  // the base being watched now
    std::array<u64, 4> addresses{};
    std::array<WatchHit, 512> hits;
};
Watch g_watch;

#ifdef _WIN32
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* context = info->ContextRecord;
    const u32 which = static_cast<u32>(context->Dr6 & 0xf);
    if (which == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u32 slot = static_cast<u32>(std::countr_zero(which));
    const u64 rip = context->Rip;
    const u32 value = *reinterpret_cast<volatile u32*>(g_watch.addresses[slot]);
    const u64 key = rip ^ (static_cast<u64>(slot) << 60);
    for (u32 i = 0; i < g_watch.hits.size(); ++i) {
        auto& hit = g_watch.hits[(static_cast<u32>(rip * 0x9e3779b1u) + i) % g_watch.hits.size()];
        u64 expected = 0;
        if (hit.rip.load() == key ||
            hit.rip.compare_exchange_strong(expected, key) || expected == key) {
            hit.slot = slot;
            hit.value = value;
            ++hit.count;
            break;
        }
    }
    context->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets the watch registers on every other thread of the process.
void ApplyWatch() {
    u64 dr7 = 0;
    for (u32 i = 0; i < g_watch.offsets.size(); ++i) {
        dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i));
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    u32 threads = 0;
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_SET_CONTEXT,
                                         FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = g_watch.addresses[0];
                context.Dr1 = g_watch.addresses[1];
                context.Dr2 = g_watch.addresses[2];
                context.Dr3 = g_watch.addresses[3];
                context.Dr7 = dr7;
                threads += SetThreadContext(thread, &context) ? 1 : 0;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    static u32 logged = 0;
    if (logged++ < 3) {
        LOG_INFO(Lib_Kernel, "Watch: {} threads watch {:#x} {:#x} {:#x} {:#x}", threads,
                 g_watch.addresses[0], g_watch.addresses[1], g_watch.addresses[2],
                 g_watch.addresses[3]);
    }
}
#endif
} // namespace

void SsuCountCalls() {
    const char* value = std::getenv("SHADPS4_SSU_COUNT");
    if (value == nullptr || *value == '\0' ||
        Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
        return;
    }
    constexpr u64 StubArea = 0x6e5000; // the executable segment's zero padding, up to the loop patch
    constexpr u64 StubAreaEnd = 0x6e7d00; // the beam spacing cave follows
    constexpr u64 DataArea = 0xcfe000; // the data segment's zero padding, up to the loop's tick
    constexpr u64 DataAreaEnd = 0xcfff00;
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    if (base == 0 || MemoryPatcher::g_eboot_image_size < DataAreaEnd + 0x100) {
        return;
    }
    const auto at = [base](u64 offset) { return reinterpret_cast<u8*>(base + offset); };
    u64 stub = StubArea;
    u64 data = DataArea;
    if (const char* w = std::getenv("SHADPS4_SSU_WATCH"); w && *w) {
        std::vector<u64> numbers;
        for (const char* s = w; *s != '\0';) {
            char* end = nullptr;
            const u64 n = std::strtoull(s, &end, 16);
            if (end == s) {
                break;
            }
            numbers.push_back(n);
            s = *end == ',' ? end + 1 : end;
        }
        if (numbers.size() >= 3) {
            g_watch.function = numbers[0];
            g_watch.pointer_offset = numbers[1];
            g_watch.offsets.assign(numbers.begin() + 2,
                                   numbers.begin() + std::min<size_t>(numbers.size(), 6));
#ifdef _WIN32
            AddVectoredExceptionHandler(1, WatchHandler);
#endif
        }
    }
    for (const char* s = value; *s != '\0';) {
        char* end = nullptr;
        const u64 function = std::strtoull(s, &end, 16);
        if (end == s) {
            break;
        }
        s = *end == ',' ? end + 1 : end;
        if (function == 0 || function >= 0x6e4e68 || stub + 0x60 > StubAreaEnd ||
            data + 24 > DataAreaEnd) {
            LOG_WARNING(Lib_Kernel, "Calls: {:#x} cannot be counted (outside the code, or too many)",
                        function);
            continue;
        }
        // Whole instructions covering the 5-byte jump, none relative or a branch.
        u64 length = 0;
        bool movable = true;
        while (length < 5 && movable) {
            ZydisDecodedInstruction inst;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            const auto status = Common::Decoder::Instance()->decodeInstruction(
                inst, operands, at(function + length), 15);
            movable = ZYAN_SUCCESS(status) && (inst.attributes & ZYDIS_ATTRIB_IS_RELATIVE) == 0 &&
                      inst.meta.category != ZYDIS_CATEGORY_COND_BR &&
                      inst.meta.category != ZYDIS_CATEGORY_UNCOND_BR &&
                      inst.meta.category != ZYDIS_CATEGORY_CALL &&
                      inst.meta.category != ZYDIS_CATEGORY_RET;
            for (u32 i = 0; movable && i < inst.operand_count; ++i) {
                movable = !(operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                            (operands[i].mem.segment == ZYDIS_REGISTER_FS ||
                             operands[i].mem.segment == ZYDIS_REGISTER_GS));
            }
            length += movable ? inst.length : 0;
        }
        if (!movable) {
            LOG_WARNING(Lib_Kernel, "Calls: sub_{:x} starts with an instruction that cannot move",
                        function);
            continue;
        }
#ifdef _WIN32
        DWORD old_stub = 0;
        DWORD old_function = 0;
        if (!VirtualProtect(at(stub), 0x60, PAGE_EXECUTE_READWRITE, &old_stub) ||
            !VirtualProtect(at(function), 16, PAGE_EXECUTE_READWRITE, &old_function)) {
            continue;
        }
#endif
        try {
            using namespace Xbyak::util;
            Xbyak::CodeGenerator c(0x60, at(stub));
            const void* count = at(data);
            const void* caller = at(data + 8);
            const void* first_argument = at(data + 16);
            c.lock();
            c.inc(c.dword[rip + count]);
            c.push(rax);
            c.mov(rax, c.qword[rsp + 8]);
            c.mov(c.qword[rip + caller], rax);
            c.pop(rax);
            c.mov(c.qword[rip + first_argument], rdi);
            c.vmovss(c.dword[rip + at(data + 4)], xmm0);
            for (u64 i = 0; i < length; ++i) {
                c.db(*at(function + i));
            }
            c.jmp(at(function + length), Xbyak::CodeGenerator::T_NEAR);
            c.ready();
            const s64 rel = static_cast<s64>(stub) - static_cast<s64>(function + 5);
            std::array<u8, 16> jump{};
            jump.fill(0x90);
            jump[0] = 0xe9;
            const s32 rel32 = static_cast<s32>(rel);
            std::memcpy(jump.data() + 1, &rel32, 4);
            std::memcpy(at(function), jump.data(), length);
            g_call_counters.push_back({function, data});
            if (function == g_watch.function) {
                g_watch.rdi_data = data + 16;
            }
            LOG_INFO(Lib_Kernel, "Calls: counting sub_{:x} ({} bytes moved to eboot+{:#x})",
                     function, length, stub);
        } catch (const std::exception& e) {
            LOG_ERROR(Lib_Kernel, "Calls: cannot hook sub_{:x}: {}", function, e.what());
        }
#ifdef _WIN32
        VirtualProtect(at(stub), 0x60, old_stub, &old_stub);
        VirtualProtect(at(function), 16, old_function, &old_function);
        FlushInstructionCache(GetCurrentProcess(), at(stub), 0x60);
        FlushInstructionCache(GetCurrentProcess(), at(function), 16);
#endif
        stub += 0x60;
        data += 24;
    }
    if (g_watch.function != 0 && g_watch.rdi_data == 0) {
        LOG_WARNING(Lib_Kernel, "Watch: sub_{:x} is not counted (SHADPS4_SSU_COUNT), no watch",
                    g_watch.function);
    }
}

// SHADPS4_SSU_BEAM=1 (diagnostic, with sub_26f920 counted): every 40 ms, the Gold Melter beams from
// the argument of the last bullet update (rdi): its beams are listed at [rdi + 0x88, rdi + 0x90),
// each beam's segments at [beam + 8, beam + 0x10); a chain starts at a live segment ([+0x18] != 0)
// with no previous one ([+8] == 0) and runs along [+0x10]; positions at +0x1c. The longest chain's segment count, length (the sum of the gaps) and reach (the
// angle between its ends, seen from the globe's centre) are summarised with the calls.
struct BeamSample {
    float length;
    float reach;
    u32 segments;
};
std::vector<BeamSample> g_beam_samples;

void SampleBeam(uintptr_t base) {
    for (const auto& counter : g_call_counters) {
        if (counter.function != 0x26f920) {
            continue;
        }
        const auto valid = [](u64 p) { return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0; };
        const auto read64 = [](u64 p) { return *reinterpret_cast<volatile u64*>(p); };
        const auto position = [](u64 s) {
            std::array<float, 3> v{};
            for (u32 i = 0; i < 3; ++i) {
                v[i] = *reinterpret_cast<volatile float*>(s + 0x1c + 4 * i);
            }
            return v;
        };
        const u64 rdi = read64(base + counter.data + 16);
        if (!valid(rdi)) {
            return;
        }
        const u64 beams = read64(rdi + 0x88);
        const u64 beams_end = read64(rdi + 0x90);
        if (!valid(beams) || beams_end < beams || beams_end - beams > 8 * 64) {
            return;
        }
        BeamSample best{0.0f, 0.0f, 0};
        std::vector<std::array<float, 3>> chain, best_chain;
        u32 chains = 0;
        u32 live = 0;
        for (u64 b = beams; b < beams_end; b += 8) {
            const u64 beam = read64(b);
            if (!valid(beam)) {
                continue;
            }
            const u64 first = read64(beam + 8);
            const u64 end = read64(beam + 0x10);
            if (!valid(first) || end < first || end - first > 8 * 8192) {
                continue;
            }
            for (u64 p = first; p < end; p += 8) {
                const u64 seg = read64(p);
                if (!valid(seg) || *reinterpret_cast<volatile u8*>(seg + 0x18) == 0) {
                    continue;
                }
                ++live;
                if (read64(seg + 8) != 0) {
                    continue;
                }
                ++chains;
                chain.assign(1, position(seg));
                float length = 0.0f;
                for (u64 s = read64(seg + 0x10); valid(s) && chain.size() < 8192; s = read64(s + 0x10)) {
                    const auto q = position(s);
                    const auto& a = chain.back();
                    length += std::sqrt((q[0] - a[0]) * (q[0] - a[0]) + (q[1] - a[1]) * (q[1] - a[1]) +
                                        (q[2] - a[2]) * (q[2] - a[2]));
                    chain.push_back(q);
                }
                const auto& a = chain.back();
                const auto& st = chain.front();
                const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                const float ls = std::sqrt(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
                const float c = (a[0] * st[0] + a[1] * st[1] + a[2] * st[2]) / std::max(la * ls, 1e-6f);
                const float reach = std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f;
                if (length > best.length) {
                    best = {length, reach, static_cast<u32>(chain.size())};
                    best_chain = chain;
                }
            }
        }
        g_beam_samples.push_back(best);
        // user/log/ssu-beam.csv: script seconds, live segments, chains, then the longest chain's
        // positions from its start ([+8] == 0) to its end.
        static FILE* csv = [] {
            const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
            return std::fopen((dir / "ssu-beam.csv").string().c_str(), "w");
        }();
        if (csv != nullptr) {
            std::fprintf(csv, "%.3f,%u,%u", Input::ScriptedInputSeconds(), live, chains);
            for (const auto& q : best_chain) {
                std::fprintf(csv, ",%.3f,%.3f,%.3f", q[0], q[1], q[2]);
            }
            std::fprintf(csv, "\n");
            std::fflush(csv);
        }
        return;
    }
}

std::string BeamSummary() {
    if (g_beam_samples.empty()) {
        return {};
    }
    auto pick = [](std::vector<float> v, double q) {
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(q * (v.size() - 1))];
    };
    std::vector<float> length, reach, segments;
    for (const auto& s : g_beam_samples) {
        length.push_back(s.length);
        reach.push_back(s.reach);
        segments.push_back(static_cast<float>(s.segments));
    }
    std::string text = fmt::format(
        "; beam ({} samples, longest chain): length p50 {:.1f} p90 {:.1f} max {:.1f} units, reach "
        "p50 {:.1f} p90 {:.1f} max {:.1f} deg, segments p50 {:g} p90 {:g} max {:g}",
        g_beam_samples.size(), pick(length, 0.5), pick(length, 0.9), pick(length, 1.0),
        pick(reach, 0.5), pick(reach, 0.9), pick(reach, 1.0), pick(segments, 0.5),
        pick(segments, 0.9), pick(segments, 1.0));
    g_beam_samples.clear();
    return text;
}

// SHADPS4_SSU_BULLETS=1 (diagnostic, with sub_26f920 counted): every 20 ms, every record of the
// bullet pool of the last bullet update (rdi): [rdi + 0xa0] records of 0x68 bytes, [rdi + 0xb0] in
// use (packed, so a slot is not an identity). Record: +0 u32, +8 type (0..0x1a, the update's jump
// table at 0x5b9b18), +0x10 state, +0x14 position, +0x20 direction, +0x2c age (s, += dt each update),
// +0x48 effect handle, +0x50/+0x54 u32. Written to user/log/ssu-bullets.csv for
// analysis\bullet_lives.py (speed, life and reach per type, independent of the update rate).
void SampleBullets(uintptr_t base) {
    for (const auto& counter : g_call_counters) {
        if (counter.function != 0x26f920) {
            continue;
        }
        const auto valid = [](u64 p) { return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0; };
        const u64 rdi = *reinterpret_cast<volatile u64*>(base + counter.data + 16);
        if (!valid(rdi)) {
            return;
        }
        const u64 records = *reinterpret_cast<volatile u64*>(rdi + 0xa0);
        const s32 count = *reinterpret_cast<volatile s32*>(rdi + 0xb0);
        if (!valid(records) || count <= 0 || count > 4096) {
            return;
        }
        static FILE* csv = [] {
            const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
            FILE* f = std::fopen((dir / "ssu-bullets.csv").string().c_str(), "w");
            if (f != nullptr) {
                std::fprintf(f, "t,slot,type,u0,state,age,px,py,pz,dx,dy,dz,effect,u50,u54\n");
            }
            return f;
        }();
        if (csv == nullptr) {
            return;
        }
        const float t = Input::ScriptedInputSeconds();
        for (s32 i = 0; i < count; ++i) {
            const u64 r = records + 0x68ull * i;
            const auto u32_at = [r](u32 off) { return *reinterpret_cast<volatile u32*>(r + off); };
            const auto f32_at = [r](u32 off) { return *reinterpret_cast<volatile float*>(r + off); };
            std::fprintf(csv, "%.3f,%d,%u,%u,%u,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%llx,%u,%u\n", t, i,
                         u32_at(8), u32_at(0), u32_at(0x10), f32_at(0x2c), f32_at(0x14), f32_at(0x18),
                         f32_at(0x1c), f32_at(0x20), f32_at(0x24), f32_at(0x28),
                         static_cast<unsigned long long>(*reinterpret_cast<volatile u64*>(r + 0x48)),
                         u32_at(0x50), u32_at(0x54));
        }
        std::fflush(csv);
        return;
    }
}

// SHADPS4_SSU_SET=<hex addr>=<float>,.. (test knob, CUSA05951): keeps debug values (eboot + addr, the
// addresses of analysis\tunables_all.tsv) at the given floats, re-written every 100 ms so the game's
// own registration or Lua cannot undo them. E.g. 9be5bc=1000: respawn shield 1000 s, a scripted ship
// that never dies (sd3__game__plr_ship__respawn_shield_time, default 3).
void SsuSetDebugValues() {
    struct Set {
        u64 address;
        float value;
    };
    static const std::vector<Set> sets = [] {
        std::vector<Set> out;
        const char* value = std::getenv("SHADPS4_SSU_SET");
        if (value == nullptr || *value == '\0' ||
            Common::ElfInfo::Instance().GameSerial() != "CUSA05951") {
            return out;
        }
        std::string_view rest{value};
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const std::string item{rest.substr(0, comma)};
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const auto eq = item.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const u64 address = std::strtoull(item.substr(0, eq).c_str(), nullptr, 16);
            const float v = std::strtof(item.substr(eq + 1).c_str(), nullptr);
            if (address < 0x400000 || address + 4 > MemoryPatcher::g_eboot_image_size) {
                LOG_WARNING(Lib_Kernel, "Debug values: {:#x} is outside the eboot, skipped", address);
                continue;
            }
            out.push_back({address, v});
            LOG_INFO(Lib_Kernel, "Debug values: eboot+{:#x} kept at {:g}", address, v);
        }
        return out;
    }();
    if (sets.empty()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    const auto now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::milliseconds{100};
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    for (const auto& s : sets) {
        auto* where = reinterpret_cast<volatile float*>(base + s.address);
        if (*where != s.value) {
            static u32 logs = 0;
            if (logs++ < 20) {
                LOG_INFO(Lib_Kernel, "Debug values: eboot+{:#x} was {:g}, set to {:g}", s.address,
                         static_cast<float>(*where), s.value);
            }
            *where = s.value;
        }
    }
}

void SsuReportCalls() {
    SsuSetDebugValues();
    if (g_call_counters.empty()) {
        return;
    }
    {
        using Clock = std::chrono::steady_clock;
        static const bool bullets_on = [] {
            const char* b = std::getenv("SHADPS4_SSU_BULLETS");
            return b != nullptr && *b == '1';
        }();
        static Clock::time_point bullets_last = Clock::now();
        const auto now = Clock::now();
        if (bullets_on && now - bullets_last >= std::chrono::milliseconds(20)) {
            bullets_last = now;
            SampleBullets(MemoryPatcher::g_eboot_address);
        }
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const auto now = Clock::now();
    static const bool beam_on = [] {
        const char* b = std::getenv("SHADPS4_SSU_BEAM");
        return b != nullptr && *b == '1';
    }();
    static Clock::time_point beam_last = now;
    if (beam_on && now - beam_last >= std::chrono::milliseconds(40)) {
        beam_last = now;
        SampleBeam(MemoryPatcher::g_eboot_address);
    }
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 5.0) {
        return;
    }
    last = now;
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    std::string text;
    for (auto& counter : g_call_counters) {
        const u32 count = *reinterpret_cast<volatile u32*>(base + counter.data);
        const u64 caller = *reinterpret_cast<volatile u64*>(base + counter.data + 8);
        const float xmm0 = *reinterpret_cast<volatile float*>(base + counter.data + 4);
        const bool in_eboot = caller >= base && caller < base + MemoryPatcher::g_eboot_image_size;
        text += fmt::format("; sub_{:x} {:.1f}/s (from {}{:#x}, xmm0 {:g})", counter.function,
                            (count - counter.reported) / seconds, in_eboot ? "eboot+" : "",
                            in_eboot ? caller - base : caller, xmm0);
        counter.reported = count;
    }
    text += BeamSummary();
    LOG_INFO(Lib_Kernel, "Calls{}", text);
#ifdef _WIN32
    if (g_watch.rdi_data != 0) {
        const u64 rdi = *reinterpret_cast<volatile u64*>(base + g_watch.rdi_data);
        // Pointer offset 0xffff: the argument itself is the base.
        const u64 watched = rdi == 0                            ? 0
                            : g_watch.pointer_offset == 0xffff ? rdi
                                : *reinterpret_cast<volatile u64*>(rdi + g_watch.pointer_offset);
        if (watched != 0) {
            for (u32 i = 0; i < 4; ++i) {
                g_watch.addresses[i] =
                    i < g_watch.offsets.size() ? watched + g_watch.offsets[i] : 0;
            }
            g_watch.watching = watched;
            ApplyWatch(); // every report: threads started since get it too
        }
        std::vector<std::tuple<u32, u64, u32, u32>> hits;
        for (auto& hit : g_watch.hits) {
            const u64 key = hit.rip.load();
            if (key != 0) {
                hits.emplace_back(hit.count.load(), key & ((1ull << 60) - 1), hit.slot.load(),
                                  hit.value.load());
            }
        }
        std::sort(hits.begin(), hits.end(), std::greater<>());
        std::string writes;
        for (const auto& [n, rip, slot, value] : hits) {
            const bool in_eboot = rip >= base && rip < base + MemoryPatcher::g_eboot_image_size;
            writes += fmt::format("; +{:#x} by {}{:#x} x{} (last {:#x})", g_watch.offsets[slot],
                                  in_eboot ? "eboot+" : "", in_eboot ? rip - base : rip, n, value);
        }
        LOG_INFO(Lib_Kernel, "Watch: sub_{:x} rdi {:#x}, base {:#x}{}", g_watch.function, rdi,
                 watched, writes);
    }
#endif
}

void SsuFollowRecenter(u32 recenters) {
    static u32 followed = 0;
    static int wait = -1; // tracker results still to wait before calibrating, -1: none due
    if (recenters != followed) {
        followed = recenters;
        // The title copies each result into one of its two records after the call; the record it
        // calibrates from then holds one from after the reset.
        wait = 3;
        return;
    }
    if (wait < 0 || --wait > 0) {
        return;
    }
    wait = -1;
    static const bool is_ssu = Common::ElfInfo::Instance().GameSerial() == "CUSA05951";
    const uintptr_t base = MemoryPatcher::g_eboot_address;
    constexpr u64 Offset = 0xcde454;   // float x, y, z: minus the position at calibration
    constexpr u64 Records = 0xcde280;  // + 0xd8 x index + 0x254: a tracker result's position
    constexpr u64 Latest = 0xce27d0;   // u32 index of the record the title calibrates from
    if (!is_ssu || base == 0 || MemoryPatcher::g_eboot_image_size < Latest + 4) {
        return;
    }
    const u32 index = *reinterpret_cast<const u32*>(base + Latest);
    if (index > 1) {
        LOG_WARNING(Lib_Kernel, "SSU calibration: record index {} is not 0 or 1, left alone",
                    index);
        return;
    }
    const auto* at = reinterpret_cast<const float*>(base + Records + 0xd8 * u64{index} + 0x254);
    auto* offset = reinterpret_cast<float*>(base + Offset);
    const auto plausible = [](const float* v) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(v[i]) || std::abs(v[i]) > 10.0f) {
                return false;
            }
        }
        return true;
    };
    if (!plausible(at) || !plausible(offset)) {
        LOG_WARNING(Lib_Kernel,
                    "SSU calibration: implausible position ({}, {}, {}) or offset ({}, {}, {}), "
                    "left alone",
                    at[0], at[1], at[2], offset[0], offset[1], offset[2]);
        return;
    }
    LOG_INFO(Lib_Kernel,
             "SSU calibration after the view reset: the head is at ({:.3f}, {:.3f}, {:.3f}) of the "
             "title's tracking; its offset was ({:.3f}, {:.3f}, {:.3f}), now the centre is here",
             at[0], at[1], at[2], offset[0], offset[1], offset[2]);
    offset[0] = -at[0];
    offset[1] = -at[1];
    offset[2] = -at[2];
}

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------
// SSU position probe (test knob): finds the player's position in memory and how far it moved.
// SHADPS4_SSU_SCAN=<a>,<b>,<until> in seconds on the input script's clock (process clock without a
// script). At <a> and <b> a background thread reads the game's mapped, readable memory for float triples
// whose length is in one of the ranges of SHADPS4_SSU_SCAN_RANGES (default 80-150,260-335: around the
// planet and flying radii and the camera distances). Those equal in both scans are watched until
// <until>; user/log/ssu-moved.csv gets every one that changed: where it was, where it ended, when it
// first and last changed and the angle between. The ship is held still between <a> and <b> and moved
// after, so its position is among them; angle x radius is the arc it flew.

namespace {

struct ScanHit {
    u64 address;
    float v[3];
};

// Scans one region; a page that goes away or loses its access mid-scan ends the region quietly.
// (No C++ objects here: __try cannot unwind them.)
u64 ScanRegion(const float* begin, u64 count, const float* bounds, int nbounds, ScanHit* out,
               u64 room) {
    u64 found = 0;
    __try {
        for (u64 i = 0; i + 2 < count && found < room; ++i) {
            const float x = begin[i], y = begin[i + 1], z = begin[i + 2];
            const float d = x * x + y * y + z * z;
            for (int k = 0; k < nbounds; k += 2) {
                if (d >= bounds[k] && d <= bounds[k + 1]) {
                    out[found].address = reinterpret_cast<u64>(begin + i);
                    out[found].v[0] = x;
                    out[found].v[1] = y;
                    out[found].v[2] = z;
                    ++found;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

bool ReadTriple(u64 address, float* v) {
    __try {
        const float* f = reinterpret_cast<const float*>(address);
        v[0] = f[0];
        v[1] = f[1];
        v[2] = f[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<ScanHit> ScanMemory(const std::vector<float>& bounds, VAddr span_lo = 0,
                                VAddr span_hi = ~VAddr{0}) {
    constexpr u64 MaxHits = 16'000'000;
    std::vector<ScanHit> hits(MaxHits);
    u64 count = 0;
    u64 scanned = 0;
    const auto start = std::chrono::steady_clock::now();
    // The game's own mappings (not the emulator's), then within each only the host pages that can be
    // read without a fault (the GPU's write and read tracking leaves some without access).
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo guest{};
    VAddr next = span_lo;
    while (count < MaxHits && next < span_hi && memory->VirtualQuery(next, 1, &guest) == 0 &&
           guest.end > next) {
        next = guest.end;
        if (!guest.is_committed || (guest.protection & 1) == 0) {
            continue;
        }
        guest.start = std::max<u64>(guest.start, span_lo);
        guest.end = std::min<u64>(guest.end, span_hi);
        MEMORY_BASIC_INFORMATION info;
        for (u64 address = guest.start; address < guest.end && count < MaxHits;) {
            if (::VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) !=
                sizeof(info)) {
                break;
            }
            const u64 region_end =
                std::min<u64>(reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize, guest.end);
            const DWORD p = info.Protect;
            const bool readable =
                info.State == MEM_COMMIT && (p & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            if (readable && region_end > address) {
                count += ScanRegion(reinterpret_cast<const float*>(address),
                                    (region_end - address) / 4, bounds.data(),
                                    static_cast<int>(bounds.size()), hits.data() + count,
                                    MaxHits - count);
                scanned += region_end - address;
            }
            if (region_end <= address) {
                break;
            }
            address = region_end;
        }
    }
    hits.resize(count);
    LOG_INFO(Lib_Kernel, "SSU probe: scan took {:.1f} s",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    LOG_INFO(Lib_Kernel, "SSU probe: scanned {} MB, {} triples of the wanted lengths{}",
             scanned >> 20, count, count == MaxHits ? " (list full: the rest went unscanned)" : "");
    return hits;
}

struct ProbeConfig {
    double at[2]{};
    double until = 0.0;
    std::vector<float> bounds; // squared lengths, pairs
};

double ProbeClock() {
    const double script = Input::ScriptedInputSeconds();
    if (script >= 0.0 || std::getenv("SHADPS4_INPUT_SCRIPT") != nullptr) {
        return script;
    }
    static const auto begin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

void RunProbe(ProbeConfig cfg) {
    const auto wait_until = [](double t) {
        while (ProbeClock() < t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    wait_until(cfg.at[0]);
    std::vector<ScanHit> stable;
    {
        const auto first = ScanMemory(cfg.bounds);
        wait_until(cfg.at[1]);
        const auto second = ScanMemory(cfg.bounds);
        // Both lists are in address order.
        size_t j = 0;
        for (const auto& hit : first) {
            while (j < second.size() && second[j].address < hit.address) {
                ++j;
            }
            if (j < second.size() && second[j].address == hit.address &&
                std::memcmp(second[j].v, hit.v, sizeof(hit.v)) == 0) {
                stable.push_back(hit);
            }
        }
    }
    LOG_INFO(Lib_Kernel, "SSU probe: {} triples unchanged between the scans; watching them",
             stable.size());
    // SHADPS4_SSU_SCAN_DUMP=1: all of them to user/log/ssu-stable.bin (u64 address, float x, y, z).
    if (const char* dump = std::getenv("SHADPS4_SSU_SCAN_DUMP"); dump && *dump == '1') {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        if (FILE* f = std::fopen((dir / "ssu-stable.bin").string().c_str(), "wb")) {
            for (const auto& hit : stable) {
                std::fwrite(&hit.address, sizeof(hit.address), 1, f);
                std::fwrite(hit.v, sizeof(hit.v), 1, f);
            }
            std::fclose(f);
        }
    }
    struct Track {
        float last[3];
        double first_change = -1.0;
        double last_change = -1.0;
        u32 changes = 0;
        std::vector<std::array<float, 3>> samples; // every 0.1 s from the first change on
        u32 first_sample = 0;
    };
    std::vector<Track> tracks(stable.size());
    for (size_t i = 0; i < stable.size(); ++i) {
        std::memcpy(tracks[i].last, stable[i].v, sizeof(tracks[i].last));
    }
    u64 passes = 0;
    for (double now = ProbeClock(); now < cfg.until; now = ProbeClock()) {
        for (size_t i = 0; i < stable.size(); ++i) {
            float v[3];
            if (!ReadTriple(stable[i].address, v)) {
                continue;
            }
            Track& t = tracks[i];
            const u32 sample = static_cast<u32>((now - cfg.at[1]) * 10.0);
            if (std::memcmp(v, t.last, sizeof(v)) != 0) {
                if (t.changes++ == 0) {
                    t.first_change = now;
                    // The value before it moved, as the sample before.
                    t.first_sample = sample > 0 ? sample - 1 : 0;
                    t.samples.push_back({t.last[0], t.last[1], t.last[2]});
                }
                std::memcpy(t.last, v, sizeof(v));
                t.last_change = now;
            }
            if (t.changes > 0 && t.samples.size() < 4000) {
                while (t.first_sample + t.samples.size() <= sample) {
                    t.samples.push_back({v[0], v[1], v[2]});
                }
            }
        }
        ++passes;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    u64 moved = 0;
    if (FILE* f = std::fopen((dir / "ssu-moved.csv").string().c_str(), "w")) {
        std::fprintf(f, "address,x0,y0,z0,x1,y1,z1,r0,r1,angle_deg,first_change,last_change,"
                        "changes\n");
        for (size_t i = 0; i < stable.size(); ++i) {
            const Track& t = tracks[i];
            if (t.changes == 0) {
                continue;
            }
            ++moved;
            const float* a = stable[i].v;
            const float* b = t.last;
            const double ra =
                std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
            const double rb =
                std::sqrt(double(b[0]) * b[0] + double(b[1]) * b[1] + double(b[2]) * b[2]);
            double c =
                (double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]) / (ra * rb);
            c = std::clamp(c, -1.0, 1.0);
            std::fprintf(f, "%llx,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.3f,%.3f,%u\n",
                         static_cast<unsigned long long>(stable[i].address), a[0], a[1], a[2],
                         b[0], b[1], b[2], ra, rb, std::acos(c) * 180.0 / 3.14159265358979,
                         t.first_change, t.last_change, t.changes);
        }
        std::fclose(f);
    }
    // Paths (0.1 s apart, on the probe's clock) of those that moved and kept their length.
    if (FILE* f = std::fopen((dir / "ssu-track.csv").string().c_str(), "w")) {
        std::fprintf(f, "address,seconds,x,y,z\n");
        for (size_t i = 0; i < stable.size(); ++i) {
            const Track& t = tracks[i];
            if (t.changes == 0) {
                continue;
            }
            const float* a = stable[i].v;
            const float* b = t.last;
            const float ra = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const float rb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (!(std::fabs(ra - rb) < 1.0f)) {
                continue;
            }
            for (size_t k = 0; k < t.samples.size(); ++k) {
                std::fprintf(f, "%llx,%.1f,%.5f,%.5f,%.5f\n",
                             static_cast<unsigned long long>(stable[i].address),
                             cfg.at[1] + (t.first_sample + k) / 10.0, t.samples[k][0],
                             t.samples[k][1], t.samples[k][2]);
            }
        }
        std::fclose(f);
    }
    LOG_INFO(Lib_Kernel, "SSU probe: done after {} passes, {} of them moved (ssu-moved.csv)",
             passes, moved);
}

// SHADPS4_SSU_TRACE=<start>,<until> (seconds on the script's clock): for things that come and go, like
// shots. From <start>, three scans in a row for float triples of the lengths in SHADPS4_SSU_TRACE_RANGES
// (default 134-146, around the flying radius); the addresses whose value changed between the scans or
// was there in only some of them are then read as fast as possible until <until>, and every change is
// written to user/log/ssu-trace.bin (records of float seconds, u32 index, float x, y, z; the first
// record of each is its value when tracing began) with the addresses in ssu-trace-addr.txt.
#pragma pack(push, 1)
struct TraceRecord {
    float t;
    u32 index;
    float v[3];
};
#pragma pack(pop)

void RunTrace(double start, double until, std::vector<float> bounds, VAddr span_lo,
              VAddr span_hi, int scans) {
    while (ProbeClock() < start) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    struct Seen {
        float v[3];
        u8 count = 0;
        bool differs = false;
    };
    const int Scans = scans;
    std::vector<u64> movers;
    {
        std::unordered_map<u64, Seen> seen;
        for (int s = 0; s < Scans; ++s) {
            const auto hits = ScanMemory(bounds, span_lo, span_hi);
            if (s == 0) {
                seen.reserve(hits.size() * 2);
            }
            for (const auto& hit : hits) {
                auto [it, fresh] = seen.try_emplace(hit.address);
                Seen& e = it->second;
                if (fresh) {
                    std::memcpy(e.v, hit.v, sizeof(e.v));
                } else if (std::memcmp(e.v, hit.v, sizeof(e.v)) != 0) {
                    e.differs = true;
                }
                ++e.count;
            }
        }
        for (const auto& [address, e] : seen) {
            if (e.differs || e.count < Scans) {
                movers.push_back(address);
            }
        }
    }
    std::sort(movers.begin(), movers.end());
    LOG_INFO(Lib_Kernel, "SSU trace: {} addresses moved or came and went over {} scans; tracing",
             movers.size(), Scans);
    constexpr size_t MaxRecords = 24'000'000;
    std::vector<TraceRecord> records;
    records.reserve(std::min<size_t>(MaxRecords, movers.size() * 64));
    std::vector<std::array<float, 3>> last(movers.size());
    const double begin = ProbeClock();
    for (size_t i = 0; i < movers.size(); ++i) {
        float v[3]{};
        ReadTriple(movers[i], v);
        last[i] = {v[0], v[1], v[2]};
        records.push_back({static_cast<float>(begin), static_cast<u32>(i), {v[0], v[1], v[2]}});
    }
    u64 passes = 0;
    for (double now = ProbeClock(); now < until && records.size() < MaxRecords;
         now = ProbeClock()) {
        for (size_t i = 0; i < movers.size(); ++i) {
            float v[3];
            if (!ReadTriple(movers[i], v)) {
                continue;
            }
            if (std::memcmp(v, last[i].data(), sizeof(v)) != 0) {
                last[i] = {v[0], v[1], v[2]};
                records.push_back(
                    {static_cast<float>(now), static_cast<u32>(i), {v[0], v[1], v[2]}});
            }
        }
        ++passes;
        std::this_thread::yield();
    }
    const double end = ProbeClock();
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    if (FILE* f = std::fopen((dir / "ssu-trace.bin").string().c_str(), "wb")) {
        std::fwrite(records.data(), sizeof(TraceRecord), records.size(), f);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((dir / "ssu-trace-addr.txt").string().c_str(), "w")) {
        for (const u64 address : movers) {
            std::fprintf(f, "%llx\n", static_cast<unsigned long long>(address));
        }
        std::fclose(f);
    }
    LOG_INFO(Lib_Kernel, "SSU trace: {:.2f} s to {:.2f} s, {} passes, {} records{}", begin, end,
             passes, records.size(), records.size() >= MaxRecords ? " (full)" : "");
}

std::vector<float> ParseRanges(const char* knob, const char* fallback, std::string& text) {
    const char* ranges = std::getenv(knob);
    text = ranges != nullptr && *ranges != '\0' ? ranges : fallback;
    std::vector<float> bounds;
    for (const char* s = text.c_str(); *s != '\0';) {
        float lo = 0.0f, hi = 0.0f;
        int used = 0;
        if (std::sscanf(s, "%f-%f%n", &lo, &hi, &used) != 2 || !(lo < hi)) {
            break;
        }
        bounds.push_back(lo * lo);
        bounds.push_back(hi * hi);
        s += used;
        if (*s == ',') {
            ++s;
        }
    }
    return bounds;
}

} // namespace

void SsuProbe() {
    static std::once_flag trace_once;
    std::call_once(trace_once, [] {
        const char* value = std::getenv("SHADPS4_SSU_TRACE");
        if (value == nullptr || *value == '\0') {
            return;
        }
        double start = 0.0, until = 0.0;
        std::string text;
        auto bounds = ParseRanges("SHADPS4_SSU_TRACE_RANGES", "134-146", text);
        if (std::sscanf(value, "%lf,%lf", &start, &until) != 2 || !(start < until) ||
            bounds.empty()) {
            LOG_WARNING(Lib_Kernel, "SSU trace: invalid SHADPS4_SSU_TRACE(_RANGES), off");
            return;
        }
        // SHADPS4_SSU_TRACE_SPAN=<lo>-<hi> (hex guest addresses) limits the scans to that range, so
        // they take a fraction of a second; SHADPS4_SSU_TRACE_SCANS (2-5, default 3) how many.
        VAddr span_lo = 0, span_hi = ~VAddr{0};
        if (const char* span = std::getenv("SHADPS4_SSU_TRACE_SPAN"); span && *span) {
            unsigned long long lo = 0, hi = 0;
            if (std::sscanf(span, "%llx-%llx", &lo, &hi) == 2 && lo < hi) {
                span_lo = lo;
                span_hi = hi;
            }
        }
        int scans = 3;
        if (const char* n = std::getenv("SHADPS4_SSU_TRACE_SCANS"); n && *n) {
            scans = std::clamp(std::atoi(n), 2, 5);
        }
        LOG_INFO(Lib_Kernel, "SSU trace: {} scans from {} s, trace until {} s, lengths {}, span {:x}-{:x}",
                 scans, start, until, text, span_lo, span_hi);
        std::thread{RunTrace, start, until, std::move(bounds), span_lo, span_hi, scans}.detach();
    });
    static std::once_flag once;
    std::call_once(once, [] {
        const char* value = std::getenv("SHADPS4_SSU_SCAN");
        if (value == nullptr || *value == '\0') {
            return;
        }
        ProbeConfig cfg;
        if (std::sscanf(value, "%lf,%lf,%lf", &cfg.at[0], &cfg.at[1], &cfg.until) != 3 ||
            !(cfg.at[0] < cfg.at[1] && cfg.at[1] < cfg.until)) {
            LOG_WARNING(Lib_Kernel, "SSU probe: invalid SHADPS4_SSU_SCAN, off");
            return;
        }
        const char* ranges = std::getenv("SHADPS4_SSU_SCAN_RANGES");
        std::string text = ranges != nullptr && *ranges != '\0' ? ranges : "80-150,260-335";
        for (const char* s = text.c_str(); *s != '\0';) {
            float lo = 0.0f, hi = 0.0f;
            int used = 0;
            if (std::sscanf(s, "%f-%f%n", &lo, &hi, &used) != 2 || !(lo < hi)) {
                break;
            }
            cfg.bounds.push_back(lo * lo);
            cfg.bounds.push_back(hi * hi);
            s += used;
            if (*s == ',') {
                ++s;
            }
        }
        if (cfg.bounds.empty()) {
            LOG_WARNING(Lib_Kernel, "SSU probe: invalid SHADPS4_SSU_SCAN_RANGES, off");
            return;
        }
        LOG_INFO(Lib_Kernel, "SSU probe: scans at {} s and {} s, watch until {} s, lengths {}",
                 cfg.at[0], cfg.at[1], cfg.until, text);
        std::thread{RunProbe, std::move(cfg)}.detach();
    });
}
#else
void SsuProbe() {}
#endif

} // namespace Libraries::Kernel::WaitStats
