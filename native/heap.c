// The native app's malloc heap, in direct memory.
//
// A native title's malloc comes from the system heap, which grows out of the
// title's *flexible* memory: only a few hundred MB, most of it taken before
// main() by the program image itself. On the console the self-test showed
// 8 MB working and 8 MB + 16 bytes failing, so SDL couldn't even create the
// screen. Titles have far more *direct* memory (about 11 GB), so - like the
// Kodi port for PS5 (shims/native-app/heap_dmem.c, GPL-2.0-or-later) - this
// reserves one large block of it at start-up and runs dlmalloc (Doug Lea,
// MIT-0, native/dlmalloc.c) inside it.
//
// The link wraps malloc, free, calloc, realloc, the aligned variants and
// malloc_usable_size (native/build.sh), so every call in the app and its
// libraries comes here. A pointer outside the block was allocated by the
// system itself (inside a system module) and goes back to the system.

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
// The PS5 target assumes every allocation is 32-byte aligned
// (__STDCPP_DEFAULT_NEW_ALIGNMENT__ is 32): the compiler stores 32 bytes at
// once (vmovaps ymm) into fresh blocks, which faulted on dlmalloc's default 16.
#define MALLOC_ALIGNMENT 32
#define MALLOC_FAILURE_ACTION errno = ENOMEM;
#define ABORT __builtin_trap()
#include "dlmalloc.c"

void* __real_malloc(size_t size);
void __real_free(void* p);
void* __real_realloc(void* p, size_t size);

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t* start);
int sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                             int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelDebugOutText(int channel, const char* text);

static mspace g_heap;
static uintptr_t g_lo, g_hi;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void heap_log(const char* fmt, long long a, long long b) {
	char line[200];
	snprintf(line, sizeof(line), fmt, a, b);
	sceKernelDebugOutText(0, line);
}

// One block of direct memory, CPU read/write (the parameters the Kodi port
// and ProsperoLight use on hardware).
static void* map_direct(size_t size) {
	static const int types[] = {12, 0};
	static const int protections[] = {0x03, 0x33};
	int64_t limit = sceKernelGetDirectMemorySize();
	for (unsigned t = 0; t < 2; t++) {
		int64_t start = -1;
		if (sceKernelAllocateDirectMemory(0, limit, size, 0x4000, types[t], &start) != 0) continue;
		for (unsigned p = 0; p < 2; p++) {
			void* address = NULL;
			if (sceKernelMapDirectMemory(&address, size, protections[p], 0, start, 0x4000) == 0 && address)
				return address;
		}
		sceKernelReleaseDirectMemory(start, size);
	}
	return NULL;
}

static void heap_init(void) {
	static const size_t sizes_mb[] = {2048, 1536, 1024, 768, 512, 256};
	for (unsigned i = 0; i < sizeof(sizes_mb) / sizeof(sizes_mb[0]); i++) {
		size_t size = sizes_mb[i] << 20;
		void* base = map_direct(size);
		if (!base) continue;
		g_heap = create_mspace_with_base(base, size, 1);
		if (!g_heap) continue;
		g_lo = (uintptr_t)base;
		g_hi = g_lo + size;
		heap_log("[stremio] heap: %lld MB of direct memory at 0x%llx\n", (long long)sizes_mb[i],
		         (long long)g_lo);
		return;
	}
	sceKernelDebugOutText(0, "[stremio] heap: no direct memory; using the small system heap\n");
}

static inline int heap_ready(void) {
	pthread_once(&g_once, heap_init);
	return g_heap != NULL;
}

static inline int ours(const void* p) {
	return (uintptr_t)p >= g_lo && (uintptr_t)p < g_hi;
}

void* __wrap_malloc(size_t size) {
	return heap_ready() ? mspace_malloc(g_heap, size) : __real_malloc(size);
}

void __wrap_free(void* p) {
	if (!p) return;
	if (ours(p))
		mspace_free(g_heap, p);
	else
		__real_free(p);  // allocated by the system
}

void* __wrap_calloc(size_t n, size_t size) {
	if (heap_ready()) return mspace_calloc(g_heap, n, size);
	if (size && n > SIZE_MAX / size) return NULL;
	void* p = __real_malloc(n * size);
	if (p) memset(p, 0, n * size);
	return p;
}

void* __wrap_realloc(void* p, size_t size) {
	if (!p) return __wrap_malloc(size);
	if (ours(p)) return mspace_realloc(g_heap, p, size);
	return __real_realloc(p, size);
}

void* __wrap_reallocf(void* p, size_t size) {
	void* q = __wrap_realloc(p, size);
	if (!q && size) __wrap_free(p);
	return q;
}

static void* aligned(size_t align, size_t size) {
	if (align <= 32) return __wrap_malloc(size);
	return heap_ready() ? mspace_memalign(g_heap, align, size) : NULL;
}

int __wrap_posix_memalign(void** out, size_t align, size_t size) {
	if (align < sizeof(void*) || (align & (align - 1)) != 0) return EINVAL;
	void* p = aligned(align, size);
	if (!p) return ENOMEM;
	*out = p;
	return 0;
}

void* __wrap_memalign(size_t align, size_t size) { return aligned(align, size); }
void* __wrap_aligned_alloc(size_t align, size_t size) { return aligned(align, size); }
void* __wrap_valloc(size_t size) { return aligned(16384, size); }

size_t __wrap_malloc_usable_size(const void* p) {
	return p && ours(p) ? mspace_usable_size(p) : 0;
}

// Start-up check, written to the kernel log: which allocation sizes work.
void heap_selftest(void) {
	static const size_t sizes[] = {64, 1 << 20, 8 << 20, 34 << 20, 128 << 20, 512 << 20};
	char line[160];
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void* p = __wrap_malloc(sizes[i]);
		void* z = __wrap_calloc(1, sizes[i]);
		snprintf(line, sizeof(line), "[stremio] heap: %zu bytes: malloc %s, calloc %s%s\n", sizes[i],
		         p ? "ok" : "FAILED", z ? "ok" : "FAILED",
		         ((uintptr_t)p | (uintptr_t)z) & 31 ? " (NOT 32-byte aligned)" : "");
		sceKernelDebugOutText(0, line);
		__wrap_free(p);
		__wrap_free(z);
	}
}
