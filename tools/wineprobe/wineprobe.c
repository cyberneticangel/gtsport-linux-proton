// wineprobe: checks the Windows memory, thread, timer and Vulkan behaviour RyGT's runtime and renderer
// depend on, so a Wine or Proton build can be judged without a game dump. Each check prints PASS,
// WARN or FAIL with a detail line; the exit code is the number of FAILs.
//
// The checks mirror core/libs/prx/libc/include/WindowsMappings.hpp (placeholders, 16 KB shared views,
// aliases), GuestArena.cpp (the fixed guest windows), CrashReport.cpp (write faults resumed from a
// vectored handler), libkernel Pthread (fully committed guest stacks), libkernel Time (high resolution
// waitable timers) and the renderer's direct memory model (VK_EXT_external_memory_host imports).
//
// Build: tools/wineprobe/build.sh. Run: tools/linux/rygt-proton.sh probe, or wine wineprobe.exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef PVOID(WINAPI* AllocateFunction)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID(WINAPI* MapFunction)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef BOOL(WINAPI* UnmapFunction)(HANDLE, PVOID, ULONG);

static AllocateFunction allocate2;
static MapFunction map3;
static UnmapFunction unmap2;
static int failures;
static FILE* logFile; // wineprobe.log next to the exe: Proton does not pass a console's output on

static void output(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    if (logFile) {
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fflush(logFile);
    }
}

static void report(const char* status, const char* name, const char* format, ...) {
    char detail[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    output("[%s] %-34s %s\n", status, name, detail);
    if (strcmp(status, "FAIL") == 0) ++failures;
}

static double now_ms(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static MEMORY_BASIC_INFORMATION query(const void* address) {
    MEMORY_BASIC_INFORMATION memory;
    memset(&memory, 0, sizeof(memory));
    VirtualQuery(address, &memory, sizeof(memory));
    return memory;
}

// 1. Host environment.
static void check_host(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const char* (*wine_get_version)(void) = (const char* (*)(void))(void*)GetProcAddress(ntdll, "wine_get_version");
    void (*wine_get_host_version)(const char**, const char**) = (void (*)(const char**, const char**))(void*)GetProcAddress(ntdll, "wine_get_host_version");
    if (wine_get_version) {
        const char* sysname = "?";
        const char* release = "?";
        if (wine_get_host_version) wine_get_host_version(&sysname, &release);
        report("PASS", "host", "Wine %s on %s %s", wine_get_version(), sysname, release);
    } else {
        report("PASS", "host", "Windows (not Wine)");
    }
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    report(system.dwAllocationGranularity == 0x10000 && system.dwPageSize == 0x1000 ? "PASS" : "WARN", "page size / granularity", "0x%lx / 0x%lx",
           system.dwPageSize, system.dwAllocationGranularity);
    // Efficiency classes per CPU set: more than one marks a hybrid CPU to the runtime and renderer.
    ULONG length = 0;
    GetSystemCpuSetInformation(NULL, 0, &length, GetCurrentProcess(), 0);
    unsigned char* sets = length ? malloc(length) : NULL;
    if (sets && GetSystemCpuSetInformation((SYSTEM_CPU_SET_INFORMATION*)sets, length, &length, GetCurrentProcess(), 0)) {
        unsigned classes[256] = {0};
        unsigned distinct = 0;
        for (ULONG offset = 0; offset < length;) {
            const SYSTEM_CPU_SET_INFORMATION* entry = (const SYSTEM_CPU_SET_INFORMATION*)(sets + offset);
            if (entry->Size == 0) break;
            if (entry->Type == CpuSetInformation && classes[entry->CpuSet.EfficiencyClass]++ == 0) ++distinct;
            offset += entry->Size;
        }
        char text[512] = "";
        for (int c = 0; c < 256; ++c)
            if (classes[c]) snprintf(text + strlen(text), sizeof(text) - strlen(text), " class %d: %u", c, classes[c]);
        report("PASS", "CPU efficiency classes", "%u distinct (%s)%s", distinct, text + 1, distinct > 1 ? " - would pass for hybrid" : "");
    }
    free(sets);
    FILE* file = fopen("Z:\\proc\\sys\\vm\\max_map_count", "r");
    if (file) {
        long count = 0;
        if (fscanf(file, "%ld", &count) == 1)
            report(count >= 1048576 ? "PASS" : "WARN", "vm.max_map_count", "%ld%s", count, count >= 1048576 ? "" : " (raise to at least 1048576)");
        fclose(file);
    }
}

// 2. The placeholder APIs WindowsMappings requires.
static int check_exports(void) {
    HMODULE kernelbase = GetModuleHandleW(L"KernelBase.dll");
    allocate2 = (AllocateFunction)(void*)GetProcAddress(kernelbase, "VirtualAlloc2");
    map3 = (MapFunction)(void*)GetProcAddress(kernelbase, "MapViewOfFile3");
    unmap2 = (UnmapFunction)(void*)GetProcAddress(kernelbase, "UnmapViewOfFile2");
    const int ok = allocate2 && map3 && unmap2;
    report(ok ? "PASS" : "FAIL", "VirtualAlloc2/MapViewOfFile3", "VirtualAlloc2=%p MapViewOfFile3=%p UnmapViewOfFile2=%p", (void*)allocate2, (void*)map3, (void*)unmap2);
    return ok;
}

// 3. Native Linux mappings inside the guest windows (Wine reports untracked ones as free).
static void list_native_mappings(uintptr_t first, uintptr_t end) {
    FILE* maps = fopen("Z:\\proc\\self\\maps", "r");
    if (!maps) return;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long stop = 0;
        if (sscanf(line, "%llx-%llx", &start, &stop) != 2) continue;
        if (stop <= first || start >= end) continue;
        MEMORY_BASIC_INFORMATION memory = query((void*)(uintptr_t)start);
        if (memory.State == MEM_FREE) {
            line[strcspn(line, "\n")] = 0;
            report("WARN", "native mapping in guest window", "%s (VirtualQuery says free)", line);
            ++found;
        }
    }
    fclose(maps);
    if (!found) report("PASS", "native mappings in guest window", "none in 0x%llx-0x%llx", (unsigned long long)first, (unsigned long long)end);
}

// 4. GuestArena: placeholders over every free region of both guest windows.
static void check_arena(void) {
    const uintptr_t windows[2][2] = {{0x0000000200000000ull, 0x000000E000000000ull}, {0x000000F000000000ull, 0x000000FC00000000ull}};
    for (int w = 0; w < 2; ++w) {
        list_native_mappings(windows[w][0], windows[w][1]);
        unsigned regions = 0;
        unsigned long long reserved = 0;
        int ok = 1;
        const double start = now_ms();
        for (uintptr_t cursor = windows[w][0]; cursor < windows[w][1];) {
            MEMORY_BASIC_INFORMATION memory = query((void*)cursor);
            uintptr_t stop = (uintptr_t)memory.BaseAddress + memory.RegionSize;
            if (stop > windows[w][1] || memory.RegionSize == 0) stop = windows[w][1];
            if (memory.State == MEM_FREE) {
                if (!allocate2(GetCurrentProcess(), (void*)cursor, stop - cursor, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
                    report("FAIL", "guest window reserve", "0x%llx+0x%llx error %lu", (unsigned long long)cursor, (unsigned long long)(stop - cursor), GetLastError());
                    ok = 0;
                    break;
                }
                ++regions;
                reserved += stop - cursor;
            }
            cursor = stop;
        }
        if (ok)
            report("PASS", "guest window reserve", "0x%llx-0x%llx: %u placeholders, %llu GiB, %.1f ms", (unsigned long long)windows[w][0], (unsigned long long)windows[w][1], regions,
                   reserved >> 30, now_ms() - start);
    }
    // GT Sport's fixed mapping must be a placeholder now.
    MEMORY_BASIC_INFORMATION fixed = query((void*)0xf3ffe00000ull);
    report(fixed.State == MEM_RESERVE ? "PASS" : "FAIL", "GT Sport fixed address 0xf3ffe00000", "state 0x%lx", fixed.State);
}

static void* placeholder(SIZE_T bytes) {
    return allocate2(GetCurrentProcess(), NULL, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
}

// WindowsMappings::split: carve [address, address + bytes) out of the placeholder that holds it.
static int split(uintptr_t address, SIZE_T bytes) {
    MEMORY_BASIC_INFORMATION memory = query((void*)address);
    memory = query(memory.AllocationBase);
    if (memory.State != MEM_RESERVE) return 0;
    const uintptr_t base = (uintptr_t)memory.BaseAddress;
    if (address != base) {
        if (!VirtualFree((void*)base, address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return 0;
        memory = query((void*)address);
    }
    if (memory.RegionSize < bytes) return 0;
    if (memory.RegionSize != bytes && !VirtualFree((void*)address, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return 0;
    return 1;
}

// 5. Placeholder split, coalesce, 16 KB views, aliases and write watch.
static HANDLE section;
static char* sectionView;
static const SIZE_T sectionBytes = 0x100000;

static void check_placeholders(void) {
    const SIZE_T page = 0x4000;
    char* base = placeholder(0x400000);
    if (!base) {
        report("FAIL", "placeholder reserve", "error %lu", GetLastError());
        return;
    }
    // Split at 16 KB, as Map does for single guest pages.
    int ok = split((uintptr_t)base + 0x10000, page);
    MEMORY_BASIC_INFORMATION memory = query(base + 0x10000);
    report(ok && memory.RegionSize == page && memory.AllocationBase == base + 0x10000 ? "PASS" : "FAIL", "split placeholder at 16 KB",
           "ok=%d RegionSize=0x%llx AllocationBase=%p (want 0x4000, %p) error %lu", ok, (unsigned long long)memory.RegionSize, memory.AllocationBase, base + 0x10000, GetLastError());

    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, (DWORD)sectionBytes, NULL);
    if (!section) {
        report("FAIL", "pagefile section", "error %lu", GetLastError());
        return;
    }
    // One 16 KB view at a 16 KB offset (Map's page loop).
    void* view = map3(section, GetCurrentProcess(), base + 0x10000, 0x4000, page, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0);
    report(view ? "PASS" : "FAIL", "MapViewOfFile3 16 KB at offset 16 KB", "%p error %lu", view, GetLastError());
    // A second view of the same pages (a guest alias) and a read-write alias (MapAlias).
    ok = split((uintptr_t)base + 0x20000, page);
    void* alias = ok ? map3(section, GetCurrentProcess(), base + 0x20000, 0x4000, page, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0) : NULL;
    void* rw = map3(section, GetCurrentProcess(), NULL, 0, 0x10000, 0, PAGE_READWRITE, NULL, 0);
    int coherent = 0;
    if (view && alias && rw) {
        DWORD previous;
        VirtualProtect(view, page, PAGE_READWRITE, &previous);
        ((volatile char*)view)[5] = 42;
        coherent = ((volatile char*)alias)[5] == 42 && ((volatile char*)rw)[0x4005] == 42;
    }
    report(coherent ? "PASS" : "FAIL", "aliased views share pages", "alias=%p rw=%p coherent=%d", alias, rw, coherent);

    // Whole multi-page view in one call (Map's fast path), then unmap preserving the placeholder.
    ok = split((uintptr_t)base + 0x40000, 0x40000);
    void* whole = ok ? map3(section, GetCurrentProcess(), base + 0x40000, 0, 0x40000, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0) : NULL;
    DWORD previous;
    int protect = whole && VirtualProtect(base + 0x44000, page, PAGE_READONLY, &previous);
    memory = query(base + 0x44000);
    report(whole && protect && memory.AllocationBase == base + 0x40000 && memory.Type == MEM_MAPPED ? "PASS" : "FAIL", "multi-page view + per-page protect",
           "view=%p AllocationBase=%p Type=0x%lx error %lu", whole, memory.AllocationBase, memory.Type, GetLastError());
    int unmapped = whole && unmap2(GetCurrentProcess(), whole, MEM_PRESERVE_PLACEHOLDER);
    memory = query(base + 0x40000);
    report(unmapped && memory.State == MEM_RESERVE && memory.RegionSize == 0x40000 ? "PASS" : "FAIL", "UnmapViewOfFile2 preserve placeholder",
           "ok=%d State=0x%lx RegionSize=0x%llx error %lu", unmapped, memory.State, (unsigned long long)memory.RegionSize, GetLastError());

    // Private memory over a placeholder, released back into one, with and without write watch.
    ok = split((uintptr_t)base + 0x100000, 0x10000);
    void* priv = ok ? allocate2(GetCurrentProcess(), base + 0x100000, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) : NULL;
    int released = priv && VirtualFree(priv, 0x10000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    report(priv && released ? "PASS" : "FAIL", "private memory replace/preserve", "alloc=%p released=%d error %lu", priv, released, GetLastError());
    ok = split((uintptr_t)base + 0x110000, 0x10000);
    char* watched = ok ? allocate2(GetCurrentProcess(), base + 0x110000, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH, PAGE_READWRITE, NULL, 0) : NULL;
    if (watched) {
        watched[0x1000] = 1;
        void* pages[16];
        ULONG_PTR count = 16;
        ULONG granularity = 0;
        const UINT result = GetWriteWatch(0, watched, 0x10000, pages, &count, &granularity);
        report(result == 0 && count == 1 ? "PASS" : "WARN", "MEM_WRITE_WATCH over placeholder", "GetWriteWatch=%u pages=%llu (only with APS5_WRITE_WATCH=1)", result,
               (unsigned long long)count);
    } else {
        report("WARN", "MEM_WRITE_WATCH over placeholder", "refused, error %lu (only with APS5_WRITE_WATCH=1)", GetLastError());
    }

    // Coalesce the adjacent placeholders at 0x40000..0x100000 back into one.
    int coalesced = VirtualFree(base + 0x40000, 0xc0000, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    memory = query(base + 0x40000);
    report(coalesced && memory.RegionSize == 0xc0000 ? "PASS" : "FAIL", "coalesce placeholders", "ok=%d RegionSize=0x%llx error %lu", coalesced,
           (unsigned long long)memory.RegionSize, GetLastError());

    // Kept for the Vulkan check: a 1 MB section view inside a placeholder, like guest direct memory.
    if (split((uintptr_t)base + 0x200000, sectionBytes))
        sectionView = map3(section, GetCurrentProcess(), base + 0x200000, 0, sectionBytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
}

// 6. Write faults resumed from a vectored handler (dirty tracking).
static char* faultPage;
static volatile LONG faults;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    char* address = (char*)record->ExceptionInformation[1];
    if (address < faultPage || address >= faultPage + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
    DWORD previous;
    VirtualProtect(faultPage, 0x1000, PAGE_READWRITE, &previous);
    InterlockedIncrement(&faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void check_faults(void) {
    faultPage = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* handler = AddVectoredExceptionHandler(1, on_fault);
    const int rounds = 2000;
    const double start = now_ms();
    for (int i = 0; i < rounds; ++i) {
        DWORD previous;
        VirtualProtect(faultPage, 0x1000, PAGE_READONLY, &previous);
        ((volatile char*)faultPage)[i & 0xfff] = (char)i;
    }
    const double each = (now_ms() - start) * 1000.0 / rounds;
    RemoveVectoredExceptionHandler(handler);
    report(faults == rounds ? (each < 20.0 ? "PASS" : "WARN") : "FAIL", "VEH write fault resume", "%ld/%d faults, %.1f us per protect+fault", faults, rounds, each);
}

// 7. Guest thread stacks: _beginthreadex with the stack size as the commit size, then the
// fully-committed check from libkernel's StartNativeThread; and the reservation-plus-commit fix.
struct StackCheck {
    SIZE_T size;
    int reserve;
    int ok;
    uintptr_t bad;
    DWORD state;
    DWORD protect;
};

static unsigned __stdcall stack_thread(void* opaque) {
    struct StackCheck* check = opaque;
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (high <= low || high - low < check->size) {
        check->bad = low;
        return 0;
    }
    if (check->reserve && !VirtualAlloc((void*)(high - check->size), check->size, MEM_COMMIT, PAGE_READWRITE)) {
        check->bad = high - check->size;
        return 0;
    }
    for (uintptr_t cursor = high - check->size; cursor < high;) {
        MEMORY_BASIC_INFORMATION memory = query((void*)cursor);
        if (memory.State != MEM_COMMIT || memory.Protect != PAGE_READWRITE || memory.RegionSize == 0) {
            check->bad = cursor;
            check->state = memory.State;
            check->protect = memory.Protect;
            return 0;
        }
        cursor = (uintptr_t)memory.BaseAddress + memory.RegionSize;
    }
    check->ok = 1;
    return 0;
}

static void check_stacks(void) {
    const SIZE_T sizes[] = {0x10000, 0x40000, 0x100000, 0x200000, 0x400000, 0x1000000};
    for (int reserve = 0; reserve < 2; ++reserve) {
        char failed[256] = "";
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            struct StackCheck check = {sizes[i], reserve, 0, 0, 0, 0};
            const SIZE_T native = reserve ? sizes[i] + 0x10000 : sizes[i];
            HANDLE thread = (HANDLE)_beginthreadex(NULL, (unsigned)native, stack_thread, &check, reserve ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
            if (thread) {
                WaitForSingleObject(thread, INFINITE);
                CloseHandle(thread);
            }
            if (!check.ok)
                snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), " %lluK(state 0x%lx prot 0x%lx)", (unsigned long long)(sizes[i] >> 10), check.state, check.protect);
        }
        report(failed[0] ? "FAIL" : "PASS", reserve ? "guest stacks, reserve + commit (fix)" : "guest stacks, commit size (current)", failed[0] ? "not fully committed:%s" : "64K-16M ok",
               failed);
    }
}

// 8. Timed waits: high resolution waitable timers against Sleep.
static void check_timers(void) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        report("WARN", "high resolution timer", "not created, error %lu", GetLastError());
        return;
    }
    const int rounds = 100;
    double worst = 0;
    double total = 0;
    for (int i = 0; i < rounds; ++i) {
        LARGE_INTEGER due;
        due.QuadPart = -5000; // 0.5 ms
        const double start = now_ms();
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        const double late = now_ms() - start - 0.5;
        total += late;
        if (late > worst) worst = late;
    }
    CloseHandle(timer);
    double sleepTotal = 0;
    for (int i = 0; i < rounds; ++i) {
        const double start = now_ms();
        Sleep(1);
        sleepTotal += now_ms() - start - 1.0;
    }
    report(total / rounds < 0.5 ? "PASS" : "WARN", "high resolution timer 0.5 ms", "average %.3f ms late, worst %.3f ms; Sleep(1) average %.3f ms late", total / rounds, worst,
           sleepTotal / rounds);
}

// 9. Vulkan: VK_EXT_external_memory_host imports of private and section memory, GPU copies between
// them, and the renderer's protection cycle over an imported range. Minimal declarations only.
typedef uint64_t VkHandle;
typedef void* VkDispatch;
typedef int VkResult;
typedef void (*PFN_void)(void);
typedef PFN_void (*PFN_GetInstanceProcAddr)(VkDispatch, const char*);

struct VkApplicationInfo { int sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion; const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion; };
struct VkInstanceCreateInfo { int sType; const void* pNext; uint32_t flags; const struct VkApplicationInfo* pApplicationInfo; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; };
struct VkDeviceQueueCreateInfo { int sType; const void* pNext; uint32_t flags; uint32_t queueFamilyIndex; uint32_t queueCount; const float* pQueuePriorities; };
struct VkDeviceCreateInfo { int sType; const void* pNext; uint32_t flags; uint32_t queueCreateInfoCount; const struct VkDeviceQueueCreateInfo* pQueueCreateInfos; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; const void* pEnabledFeatures; };
struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; };
struct VkExternalMemoryHostProperties { int sType; void* pNext; uint64_t minImportedHostPointerAlignment; };
struct VkPhysicalDeviceProperties2 { int sType; void* pNext; struct { uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType; char deviceName[256]; unsigned char rest[1024]; } properties; };
struct VkMemoryHostPointerProperties { int sType; void* pNext; uint32_t memoryTypeBits; };
struct VkImportMemoryHostPointerInfo { int sType; const void* pNext; uint32_t handleType; void* pHostPointer; };
struct VkMemoryAllocateInfo { int sType; const void* pNext; uint64_t allocationSize; uint32_t memoryTypeIndex; };
struct VkExternalMemoryBufferCreateInfo { int sType; const void* pNext; uint32_t handleTypes; };
struct VkBufferCreateInfo { int sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; };
struct VkMemoryRequirements { uint64_t size; uint64_t alignment; uint32_t memoryTypeBits; };
struct VkCommandPoolCreateInfo { int sType; const void* pNext; uint32_t flags; uint32_t queueFamilyIndex; };
struct VkCommandBufferAllocateInfo { int sType; const void* pNext; VkHandle commandPool; uint32_t level; uint32_t commandBufferCount; };
struct VkCommandBufferBeginInfo { int sType; const void* pNext; uint32_t flags; const void* pInheritanceInfo; };
struct VkBufferCopy { uint64_t srcOffset; uint64_t dstOffset; uint64_t size; };
struct VkSubmitInfo { int sType; const void* pNext; uint32_t waitSemaphoreCount; const VkHandle* pWaitSemaphores; const uint32_t* pWaitDstStageMask; uint32_t commandBufferCount; const VkDispatch* pCommandBuffers; uint32_t signalSemaphoreCount; const VkHandle* pSignalSemaphores; };
struct VkFenceCreateInfo { int sType; const void* pNext; uint32_t flags; };

#define HOST_ALLOCATION_BIT 0x00000080u

static struct {
    PFN_GetInstanceProcAddr getInstance;
    VkDispatch instance, physical, device, queue, command;
    VkHandle pool, fence;
    VkResult (*CreateInstance)(const struct VkInstanceCreateInfo*, const void*, VkDispatch*);
    VkResult (*EnumeratePhysicalDevices)(VkDispatch, uint32_t*, VkDispatch*);
    VkResult (*EnumerateDeviceExtensionProperties)(VkDispatch, const char*, uint32_t*, struct VkExtensionProperties*);
    void (*GetPhysicalDeviceProperties2)(VkDispatch, struct VkPhysicalDeviceProperties2*);
    VkResult (*CreateDevice)(VkDispatch, const struct VkDeviceCreateInfo*, const void*, VkDispatch*);
    PFN_void (*GetDeviceProcAddr)(VkDispatch, const char*);
    VkResult (*GetMemoryHostPointerProperties)(VkDispatch, uint32_t, const void*, struct VkMemoryHostPointerProperties*);
    VkResult (*AllocateMemory)(VkDispatch, const struct VkMemoryAllocateInfo*, const void*, VkHandle*);
    void (*FreeMemory)(VkDispatch, VkHandle, const void*);
    VkResult (*CreateBuffer)(VkDispatch, const struct VkBufferCreateInfo*, const void*, VkHandle*);
    void (*DestroyBuffer)(VkDispatch, VkHandle, const void*);
    void (*GetBufferMemoryRequirements)(VkDispatch, VkHandle, struct VkMemoryRequirements*);
    VkResult (*BindBufferMemory)(VkDispatch, VkHandle, VkHandle, uint64_t);
    void (*GetDeviceQueue)(VkDispatch, uint32_t, uint32_t, VkDispatch*);
    VkResult (*CreateCommandPool)(VkDispatch, const struct VkCommandPoolCreateInfo*, const void*, VkHandle*);
    VkResult (*AllocateCommandBuffers)(VkDispatch, const struct VkCommandBufferAllocateInfo*, VkDispatch*);
    VkResult (*BeginCommandBuffer)(VkDispatch, const struct VkCommandBufferBeginInfo*);
    VkResult (*EndCommandBuffer)(VkDispatch);
    VkResult (*ResetCommandBuffer)(VkDispatch, uint32_t);
    void (*CmdCopyBuffer)(VkDispatch, VkHandle, VkHandle, uint32_t, const struct VkBufferCopy*);
    VkResult (*CreateFence)(VkDispatch, const struct VkFenceCreateInfo*, const void*, VkHandle*);
    VkResult (*ResetFences)(VkDispatch, uint32_t, const VkHandle*);
    VkResult (*WaitForFences)(VkDispatch, uint32_t, const VkHandle*, uint32_t, uint64_t);
    VkResult (*QueueSubmit)(VkDispatch, uint32_t, const struct VkSubmitInfo*, VkHandle);
} vk;

#define LOAD_FN(field, function) do { PFN_void loaded = (function); memcpy(&vk.field, &loaded, sizeof(loaded)); } while (0)
#define INSTANCE_FN(name) LOAD_FN(name, vk.getInstance(vk.instance, "vk" #name))
#define DEVICE_FN(name) LOAD_FN(name, vk.GetDeviceProcAddr(vk.device, "vk" #name))

// Imports [pointer, pointer + bytes) and wraps it in a transfer buffer; 0 when the driver refuses.
static VkHandle import_buffer(void* pointer, uint64_t bytes, VkHandle* memory, const char** why, VkResult* code) {
    struct VkMemoryHostPointerProperties properties = {1000178001, NULL, 0};
    *code = vk.GetMemoryHostPointerProperties(vk.device, HOST_ALLOCATION_BIT, pointer, &properties);
    if (*code != 0 || properties.memoryTypeBits == 0) {
        *why = "vkGetMemoryHostPointerPropertiesEXT";
        return 0;
    }
    struct VkExternalMemoryBufferCreateInfo external = {1000072000, NULL, HOST_ALLOCATION_BIT};
    struct VkBufferCreateInfo info = {12, &external, 0, bytes, 0x1 | 0x2, 0, 0, NULL};
    VkHandle buffer = 0;
    if ((*code = vk.CreateBuffer(vk.device, &info, NULL, &buffer)) != 0) {
        *why = "vkCreateBuffer";
        return 0;
    }
    struct VkMemoryRequirements requirements;
    vk.GetBufferMemoryRequirements(vk.device, buffer, &requirements);
    uint32_t bits = properties.memoryTypeBits & requirements.memoryTypeBits;
    uint32_t type = 0;
    while (bits && !(bits & (1u << type))) ++type;
    struct VkImportMemoryHostPointerInfo import = {1000178000, NULL, HOST_ALLOCATION_BIT, pointer};
    struct VkMemoryAllocateInfo allocate = {5, &import, bytes, type};
    if (!bits || (*code = vk.AllocateMemory(vk.device, &allocate, NULL, memory)) != 0) {
        *why = bits ? "vkAllocateMemory (import)" : "no common memory type";
        vk.DestroyBuffer(vk.device, buffer, NULL);
        return 0;
    }
    if ((*code = vk.BindBufferMemory(vk.device, buffer, *memory, 0)) != 0) {
        *why = "vkBindBufferMemory";
        vk.DestroyBuffer(vk.device, buffer, NULL);
        vk.FreeMemory(vk.device, *memory, NULL);
        return 0;
    }
    return buffer;
}

// Copies bytes from src to dst on the GPU; the result code of the wait.
static VkResult gpu_copy(VkHandle src, VkHandle dst, uint64_t bytes) {
    struct VkCommandBufferBeginInfo begin = {42, NULL, 1, NULL};
    struct VkBufferCopy region = {0, 0, bytes};
    vk.ResetCommandBuffer(vk.command, 0);
    vk.BeginCommandBuffer(vk.command, &begin);
    vk.CmdCopyBuffer(vk.command, src, dst, 1, &region);
    vk.EndCommandBuffer(vk.command);
    struct VkSubmitInfo submit = {4, NULL, 0, NULL, NULL, 1, &vk.command, 0, NULL};
    vk.ResetFences(vk.device, 1, &vk.fence);
    VkResult result = vk.QueueSubmit(vk.queue, 1, &submit, vk.fence);
    if (result != 0) return result;
    return vk.WaitForFences(vk.device, 1, &vk.fence, 1, 5000000000ull);
}

static int setup_vulkan(void) {
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) {
        report("FAIL", "vulkan-1.dll", "not loadable, error %lu", GetLastError());
        return 0;
    }
    vk.getInstance = (PFN_GetInstanceProcAddr)(void*)GetProcAddress(loader, "vkGetInstanceProcAddr");
    INSTANCE_FN(CreateInstance);
    struct VkApplicationInfo app = {0, NULL, "wineprobe", 1, "RyGT", 1, (1u << 22) | (2u << 12)};
    struct VkInstanceCreateInfo info = {1, NULL, 0, &app, 0, NULL, 0, NULL};
    VkResult result = vk.CreateInstance ? vk.CreateInstance(&info, NULL, &vk.instance) : -1;
    if (result != 0) {
        report("FAIL", "vkCreateInstance", "result %d", result);
        return 0;
    }
    INSTANCE_FN(EnumeratePhysicalDevices);
    INSTANCE_FN(EnumerateDeviceExtensionProperties);
    INSTANCE_FN(GetPhysicalDeviceProperties2);
    INSTANCE_FN(CreateDevice);
    INSTANCE_FN(GetDeviceProcAddr);
    VkDispatch devices[8];
    uint32_t count = 8;
    vk.EnumeratePhysicalDevices(vk.instance, &count, devices);
    if (count == 0) {
        report("FAIL", "Vulkan device", "none");
        return 0;
    }
    // The first discrete GPU, as the renderer prefers.
    vk.physical = devices[0];
    struct VkExternalMemoryHostProperties host = {1000178002, NULL, 0};
    struct VkPhysicalDeviceProperties2 properties;
    for (uint32_t i = 0; i < count; ++i) {
        memset(&properties, 0, sizeof(properties));
        properties.sType = 1000059001;
        vk.GetPhysicalDeviceProperties2(devices[i], &properties);
        if (properties.properties.deviceType == 2) {
            vk.physical = devices[i];
            break;
        }
    }
    memset(&properties, 0, sizeof(properties));
    properties.sType = 1000059001;
    properties.pNext = &host;
    vk.GetPhysicalDeviceProperties2(vk.physical, &properties);
    report("PASS", "Vulkan device", "%s (vendor 0x%04x, Vulkan %u.%u, driver 0x%x)", properties.properties.deviceName, properties.properties.vendorID,
           properties.properties.apiVersion >> 22, (properties.properties.apiVersion >> 12) & 0x3ff, properties.properties.driverVersion);

    uint32_t extensions = 0;
    vk.EnumerateDeviceExtensionProperties(vk.physical, NULL, &extensions, NULL);
    struct VkExtensionProperties* list = calloc(extensions, sizeof(*list));
    vk.EnumerateDeviceExtensionProperties(vk.physical, NULL, &extensions, list);
    int hostMemory = 0;
    for (uint32_t i = 0; i < extensions; ++i)
        if (strcmp(list[i].extensionName, "VK_EXT_external_memory_host") == 0) hostMemory = 1;
    free(list);
    report(hostMemory ? "PASS" : "FAIL", "VK_EXT_external_memory_host", hostMemory ? "available, import alignment 0x%llx" : "missing: the direct renderer cannot run",
           (unsigned long long)host.minImportedHostPointerAlignment);
    if (!hostMemory) return 0;

    const float priority = 1.0f;
    struct VkDeviceQueueCreateInfo queue = {2, NULL, 0, 0, 1, &priority};
    const char* enabled[] = {"VK_EXT_external_memory_host"};
    struct VkDeviceCreateInfo device = {3, NULL, 0, 1, &queue, 0, NULL, 1, enabled, NULL};
    if ((result = vk.CreateDevice(vk.physical, &device, NULL, &vk.device)) != 0) {
        report("FAIL", "vkCreateDevice", "result %d", result);
        return 0;
    }
    LOAD_FN(GetMemoryHostPointerProperties, vk.GetDeviceProcAddr(vk.device, "vkGetMemoryHostPointerPropertiesEXT"));
    DEVICE_FN(AllocateMemory);
    DEVICE_FN(FreeMemory);
    DEVICE_FN(CreateBuffer);
    DEVICE_FN(DestroyBuffer);
    DEVICE_FN(GetBufferMemoryRequirements);
    DEVICE_FN(BindBufferMemory);
    DEVICE_FN(GetDeviceQueue);
    DEVICE_FN(CreateCommandPool);
    DEVICE_FN(AllocateCommandBuffers);
    DEVICE_FN(BeginCommandBuffer);
    DEVICE_FN(EndCommandBuffer);
    DEVICE_FN(ResetCommandBuffer);
    DEVICE_FN(CmdCopyBuffer);
    DEVICE_FN(CreateFence);
    DEVICE_FN(ResetFences);
    DEVICE_FN(WaitForFences);
    DEVICE_FN(QueueSubmit);
    vk.GetDeviceQueue(vk.device, 0, 0, &vk.queue);
    struct VkCommandPoolCreateInfo pool = {39, NULL, 0x2, 0};
    vk.CreateCommandPool(vk.device, &pool, NULL, &vk.pool);
    struct VkCommandBufferAllocateInfo command = {40, NULL, vk.pool, 0, 1};
    vk.AllocateCommandBuffers(vk.device, &command, &vk.command);
    struct VkFenceCreateInfo fence = {8, NULL, 0};
    vk.CreateFence(vk.device, &fence, NULL, &vk.fence);
    return 1;
}

static void check_vulkan(void) {
    if (!setup_vulkan()) return;
    const uint64_t bytes = 0x10000;
    char* target = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    char* source = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const char* why = "";
    VkResult code = 0;
    VkHandle targetMemory = 0;
    VkHandle sourceMemory = 0;
    VkHandle dst = import_buffer(target, bytes, &targetMemory, &why, &code);
    report(dst ? "PASS" : "FAIL", "import VirtualAlloc memory", dst ? "ok" : "%s failed, result %d", why, code);
    if (!dst) return;
    VkHandle src = import_buffer(source, bytes, &sourceMemory, &why, &code);
    if (src) {
        memset(source, 0x5a, bytes);
        const VkResult result = gpu_copy(src, dst, bytes);
        report(result == 0 && target[bytes - 1] == 0x5a ? "PASS" : "FAIL", "GPU copy private -> private", "wait result %d, byte 0x%02x", result, (unsigned char)target[bytes - 1]);
    }

    if (!sectionView) {
        report("FAIL", "import section view", "no section view (placeholder checks failed)");
        return;
    }
    // Guest direct memory: a pagefile section view inside a placeholder, and a read-write alias of it.
    VkHandle viewMemory = 0;
    VkHandle view = import_buffer(sectionView, bytes, &viewMemory, &why, &code);
    report(view ? "PASS" : "FAIL", "import section view (placeholder)", view ? "ok" : "%s failed, result %d: the direct renderer must fall back to copies", why, code);
    char* alias = map3(section, GetCurrentProcess(), NULL, 0, sectionBytes, 0, PAGE_READWRITE, NULL, 0);
    VkHandle aliasMemory = 0;
    VkHandle aliasBuffer = alias ? import_buffer(alias, bytes, &aliasMemory, &why, &code) : 0;
    report(aliasBuffer ? "PASS" : "FAIL", "import read-write alias view", aliasBuffer ? "ok" : "%s failed, result %d (GuestArenaMapAlias)", why, code);
    VkHandle imported = aliasBuffer ? aliasBuffer : view;
    if (!imported) return;
    memset(sectionView, 0x33, bytes);
    VkResult result = gpu_copy(imported, dst, bytes);
    report(result == 0 && target[17] == 0x33 ? "PASS" : "FAIL", "GPU copy section -> private", "wait result %d, byte 0x%02x", result, (unsigned char)target[17]);
    // The renderer write-protects the game's view while the GPU reads the alias, then restores it.
    DWORD previous;
    VirtualProtect(sectionView, bytes, PAGE_READONLY, &previous);
    VirtualProtect(sectionView, bytes, PAGE_NOACCESS, &previous);
    VirtualProtect(sectionView, bytes, PAGE_READWRITE, &previous);
    memset(sectionView, 0x44, bytes);
    result = gpu_copy(imported, dst, bytes);
    report(result == 0 && target[17] == 0x44 ? "PASS" : "FAIL", "GPU copy after protection cycle", "wait result %d, byte 0x%02x (-4 = device lost)", result, (unsigned char)target[17]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (length > 4 && length < MAX_PATH) {
        strcpy(path + length - 4, ".log");
        logFile = fopen(path, "w");
    }
    output("RyGT wineprobe\n");
    check_host();
    if (check_exports()) {
        check_arena();
        check_placeholders();
    }
    check_faults();
    check_stacks();
    check_timers();
    if (allocate2) check_vulkan();
    output("%d failure(s)\n", failures);
    return failures;
}
