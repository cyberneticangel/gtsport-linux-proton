// rygt-userptr (experimental, RYGT_USERPTR=1): lets the direct renderer import guest memory on
// AMD GPUs under Proton. See docs/gt-sport/LINUX.md for why it is off by default.
//
// The renderer imports guest memory with VK_EXT_external_memory_host. RADV turns that into an
// amdgpu userptr, and libdrm_amdgpu always asks for AMDGPU_GEM_USERPTR_ANONONLY, so the kernel
// refuses memory backed by a file. Wine backs the game's memory with shared file mappings
// (sections), so every import was refused and the renderer copied every buffer instead. The
// kernel can import that memory without the flag; this library, loaded with LD_PRELOAD, clears
// it on that one ioctl and leaves every other call alone.
//
// Build: cc -O2 -shared -fPIC -o rygt-userptr.so rygt-userptr.c -ldl (rygt-proton.sh does this).
// RYGT_USERPTR_DEBUG=<file> logs each import to that file.
// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DRM_AMDGPU_GEM_USERPTR 0x11
#define AMDGPU_GEM_USERPTR_ANONONLY (1 << 1)

struct drm_amdgpu_gem_userptr {
    uint64_t addr;
    uint64_t size;
    uint32_t flags;
    uint32_t handle;
};

typedef int (*CommandWriteRead)(int fd, unsigned long index, void* data, unsigned long size);

static CommandWriteRead RealCommandWriteRead(void) {
    static CommandWriteRead real;
    if (!real) {
        // libdrm comes with the Vulkan driver, which the loader opens with local scope, so
        // RTLD_NEXT does not see it: look the function up in the library itself.
        void* drm = dlopen("libdrm.so.2", RTLD_LAZY | RTLD_NOLOAD);
        if (!drm) drm = dlopen("libdrm.so.2", RTLD_LAZY);
        if (drm) real = (CommandWriteRead)dlsym(drm, "drmCommandWriteRead");
        if (!real) abort();
    }
    return real;
}

static FILE* DebugLog(void) {
    static int opened;
    static FILE* log;
    if (!opened) {
        opened = 1;
        const char* path = getenv("RYGT_USERPTR_DEBUG");
        if (path && (log = fopen(path, "a")) != NULL) setvbuf(log, NULL, _IONBF, 0);
    }
    return log;
}

int drmCommandWriteRead(int fd, unsigned long index, void* data, unsigned long size) {
    if (index != DRM_AMDGPU_GEM_USERPTR || size != sizeof(struct drm_amdgpu_gem_userptr)) {
        return RealCommandWriteRead()(fd, index, data, size);
    }
    struct drm_amdgpu_gem_userptr* args = data;
    args->flags &= ~AMDGPU_GEM_USERPTR_ANONONLY;
    const int result = RealCommandWriteRead()(fd, index, data, size);
    FILE* log = DebugLog();
    if (log) {
        fprintf(log, "rygt-userptr: %#llx+%#llx flags %#x -> %d\n", (unsigned long long)args->addr,
                (unsigned long long)args->size, args->flags, result);
    }
    return result;
}
