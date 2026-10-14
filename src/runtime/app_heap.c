// ps5-homebrew-ui - Process-lifetime heap for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/app_heap.c. tools/build.sh links with
// --wrap for the malloc family whenever this file exists, routing allocations
// into a fixed sceLibcMspace that is never unmapped.
//
// spot5: the heap lives in direct memory (1 GiB), the pool native PS5
// ports use for big allocations: a native app's flexible memory (where plain
// mmap comes from) is only ~190-448 MiB, which is why 512 and 384 were
// refused. Recipe from PS5LM docs/RESEARCH.md (PS5SX2, PS5CEMU, ProsperoAI):
// type 12, 2 MiB aligned, mapped at an address hint far from the window the
// GPU driver maps into later. If direct memory says no, the heap falls back
// to the largest flexible size that leaves kHeadroom for thread stacks and
// the GL runtime (128 MiB was the kit's fixed choice).

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *address, size_t size);
void __real_free(void *address);
int __real_posix_memalign(void **address, size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void *address);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags);
int sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len, size_t alignment,
                                  int memory_type, off_t *phys_out);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys, size_t alignment);
int sceKernelReleaseDirectMemory(off_t start, size_t len);
size_t sceKernelGetDirectMemorySize(void);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment, size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

/* Fixed, process-lifetime heap: do not unmap underneath late C++ destructors. */
static const size_t kHeapSizes[] = {512u << 20, 384u << 20, 256u << 20, 128u << 20};
static const size_t kHeadroom = 64u << 20;
static size_t hui_heap_size;
static int hui_heap_direct; /* 1: the heap is direct memory */

static const size_t kDirectHeapSize = 1024u << 20;
static const size_t kDirectAlign = 2u << 20;
static const int kDirectType = 12;
#define HUI_DIRECT_HINT ((void *)0x1000000000ull) /* 64 GiB: clear of the GPU driver's window */

static void *hui_heap_map_direct(size_t size)
{
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, kDirectAlign, kDirectType,
                                      &phys) != 0)
        return MAP_FAILED;
    void *address = HUI_DIRECT_HINT;
    if (sceKernelMapDirectMemory(&address, size, PROT_READ | PROT_WRITE, 0, phys, kDirectAlign) != 0)
    {
        sceKernelReleaseDirectMemory(phys, size);
        return MAP_FAILED;
    }
    return address;
}

static atomic_int hui_heap_state;
static void *hui_heap_base;
static void *hui_heap_mspace;
/* Owned-heap usable bytes only; relaxed counters are observations, not fences. */
static atomic_size_t hui_heap_live_bytes, hui_heap_peak_bytes, hui_heap_blocks;
static atomic_size_t hui_heap_failures;

static void hui_heap_resize_stats(size_t before, size_t after)
{
    size_t live = after >= before ? atomic_fetch_add_explicit(&hui_heap_live_bytes, after - before,
                                                              memory_order_relaxed) +
                                        after - before
                                  : atomic_fetch_sub_explicit(&hui_heap_live_bytes, before - after,
                                                              memory_order_relaxed) -
                                        (before - after);
    size_t peak = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&hui_heap_peak_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void *hui_heap_record_allocation(void *address, int nonzero)
{
    if (address)
    {
        hui_heap_resize_stats(0, sceLibcMspaceMallocUsableSize(address));
        atomic_fetch_add_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    }
    else if (nonzero)
    {
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    }
    return address;
}

static int hui_heap_ready(void)
{
    int state = atomic_load_explicit(&hui_heap_state, memory_order_acquire);
    if (state == 2)
        return 1;
    if (state != 0)
        return 0;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&hui_heap_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;

    // The first size that also leaves the headroom free (probed by mapping
    // both, then giving the headroom back; this runs before any thread).
    /* Escape hatch: an empty file /app0/heap-flexible (put there over FTP)
     * keeps the heap in flexible memory, in case direct memory ever stops
     * the app from starting. */
    /* fopen, not access(): access() reports files under /app0 missing on the console. */
    FILE *flexible = fopen("/app0/heap-flexible", "rb");
    if (flexible)
        fclose(flexible);
    void *base = flexible ? MAP_FAILED : hui_heap_map_direct(kDirectHeapSize);
    if (base != MAP_FAILED)
    {
        hui_heap_size = kDirectHeapSize;
        hui_heap_direct = 1;
    }
    for (size_t i = 0; i < sizeof(kHeapSizes) / sizeof(kHeapSizes[0]) && base == MAP_FAILED; ++i)
    {
        const size_t size = kHeapSizes[i];
        void *probe = mmap(NULL, size + kHeadroom, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (probe == MAP_FAILED)
            continue;
        munmap(probe, size + kHeadroom);
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base != MAP_FAILED)
            hui_heap_size = size;
    }
    if (base == MAP_FAILED)
    {
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    hui_heap_base = base;
    hui_heap_mspace = sceLibcMspaceCreate("ps5-homebrew-ui", base, hui_heap_size, 0);
    if (hui_heap_mspace == NULL)
    {
        hui_heap_base = NULL;
        munmap(base, hui_heap_size);
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
    return 1;
}

static int hui_heap_owns(const void *address)
{
    /* Acquire publication before reading non-atomic heap metadata. */
    if (atomic_load_explicit(&hui_heap_state, memory_order_acquire) != 2)
        return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)hui_heap_base;
    return value >= base && value - base < hui_heap_size;
}

void *__wrap_malloc(size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceMalloc(hui_heap_mspace, size), size != 0)
               : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceCalloc(hui_heap_mspace, count, size),
                                            count != 0 && size != 0)
               : __real_calloc(count, size);
}

void *__wrap_realloc(void *address, size_t size)
{
    if (address == NULL)
        return __wrap_malloc(size);
    if (!hui_heap_owns(address))
        return __real_realloc(address, size);
    size_t before = sceLibcMspaceMallocUsableSize(address);
    void *result = sceLibcMspaceRealloc(hui_heap_mspace, address, size);
    if (result)
        hui_heap_resize_stats(before, sceLibcMspaceMallocUsableSize(result));
    else if (size)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

void __wrap_free(void *address)
{
    if (hui_heap_owns(address))
    {
        hui_heap_resize_stats(sceLibcMspaceMallocUsableSize(address), 0);
        atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
        sceLibcMspaceFree(hui_heap_mspace, address);
    }
    else
    {
        __real_free(address);
    }
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size)
{
    if (!hui_heap_ready())
        return __real_posix_memalign(address, alignment, size);
    int result = sceLibcMspacePosixMemalign(hui_heap_mspace, address, alignment, size);
    if (result == 0)
        hui_heap_record_allocation(*address, size != 0);
    else if (result == ENOMEM)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

size_t __wrap_malloc_usable_size(const void *address)
{
    return hui_heap_owns(address) ? sceLibcMspaceMallocUsableSize(address)
                                  : __real_malloc_usable_size(address);
}

/* The heap's size (0 before the first allocation or if it could not be made). */
size_t hui_heap_capacity(void)
{
    return hui_heap_size;
}

/* 1 when the heap is direct memory, 0 when it is flexible memory. */
int hui_heap_is_direct(void)
{
    return hui_heap_direct;
}

/* Diagnostics: current heap counters for logs and the diagnostics screen. */
void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures)
{
    if (live_bytes)
        *live_bytes = atomic_load_explicit(&hui_heap_live_bytes, memory_order_relaxed);
    if (peak_bytes)
        *peak_bytes = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    if (blocks)
        *blocks = atomic_load_explicit(&hui_heap_blocks, memory_order_relaxed);
    if (failures)
        *failures = atomic_load_explicit(&hui_heap_failures, memory_order_relaxed);
}
