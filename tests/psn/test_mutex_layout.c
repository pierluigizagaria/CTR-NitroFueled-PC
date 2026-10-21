/* Exercise the real guest NID entry points without a game image or renderer.
 * Track the runtime's requested allocation size before reproducing the guest
 * libc flag write: an undersized old layout fails safely, without an overflow. */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "runtime.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "mutex layout: %s (line %d)\n", #condition, __LINE__); \
        exit(1); \
    } \
} while (0)

enum { GUARD_BYTES = 32, GUEST_VIEW_BYTES = 0x100, GUEST_FLAG_OFFSET = 0x20 };
_Static_assert(GUARD_BYTES % _Alignof(max_align_t) == 0, "preserve malloc alignment");
typedef struct { unsigned char *raw, *payload; size_t size; } Allocation;
static Allocation fixture_allocations[64];
static size_t fixture_allocation_count;

static Allocation *fixture_find(const void *pointer) {
    for (size_t i = 0; i < fixture_allocation_count; ++i)
        if (fixture_allocations[i].payload == pointer) return &fixture_allocations[i];
    CHECK(0 && "unknown runtime allocation");
    return NULL;
}

static void fixture_check_guards(const Allocation *allocation) {
    for (size_t i = 0; i < GUARD_BYTES; ++i) {
        CHECK(allocation->raw[i] == 0x6b);
        CHECK(allocation->payload[allocation->size + i] == 0x6b);
    }
}

static void *fixture_malloc(size_t size) {
    CHECK(fixture_allocation_count < sizeof(fixture_allocations) / sizeof(fixture_allocations[0]));
    CHECK(size <= SIZE_MAX - 2 * GUARD_BYTES);
    unsigned char *raw = malloc(size + 2 * GUARD_BYTES);
    CHECK(raw != NULL);
    memset(raw, 0x6b, size + 2 * GUARD_BYTES);
    unsigned char *payload = raw + GUARD_BYTES;
    memset(payload, 0xa5, size); /* Uninitialized guest fields must not pass by chance. */
    fixture_allocations[fixture_allocation_count++] = (Allocation){raw, payload, size};
    return payload;
}

static void *fixture_calloc(size_t count, size_t size) {
    CHECK(!count || size <= SIZE_MAX / count);
    void *payload = fixture_malloc(count * size);
    memset(payload, 0, count * size);
    return payload;
}

static void fixture_free(void *pointer) {
    if (!pointer) return;
    Allocation *allocation = fixture_find(pointer);
    fixture_check_guards(allocation);
    free(allocation->raw);
    allocation->raw = allocation->payload = NULL;
}

/* Wrap only the runtime allocations; pthreads uses the actual host allocator. */
#define malloc fixture_malloc
#define calloc fixture_calloc
#define free fixture_free
#include "../../src/runtime_mutex.c"
#undef malloc
#undef calloc
#undef free

void runtime_wait_note(int kind, uint64_t ns) { (void)kind; (void)ns; }

typedef int32_t (ABI *AttrInit)(GuestAttr **);
typedef int32_t (ABI *AttrType)(GuestAttr **, int);
typedef int32_t (ABI *AttrDestroy)(GuestAttr **);
typedef int32_t (ABI *SceInit)(GuestMutex **, GuestAttr **, const char *);
typedef int32_t (ABI *PosixInit)(GuestMutex **, GuestAttr **);
typedef int32_t (ABI *MutexCall)(GuestMutex **);
typedef struct {
    AttrInit attr_init;
    AttrType attr_type;
    AttrDestroy attr_destroy;
    SceInit sce_init;
    PosixInit posix_init;
    MutexCall lock, trylock, unlock, destroy;
    int32_t busy, invalid;
} GuestApi;

static uintptr_t fixture_resolve(const char *nid) {
    uintptr_t address = runtime_mutex_resolve(nid);
    CHECK(address != 0);
    return address;
}

static GuestApi fixture_api(int posix) {
    if (posix) return (GuestApi){
        (AttrInit)fixture_resolve("dQHWEsJtoE4#libScePosix"),
        (AttrType)fixture_resolve("mDmgMOGVUqg#libScePosix"),
        (AttrDestroy)fixture_resolve("HF7lK46xzjY#libScePosix"),
        NULL, (PosixInit)fixture_resolve("ttHNfU+qDBU#libScePosix"),
        (MutexCall)fixture_resolve("7H0iTOciTLo#libScePosix"),
        (MutexCall)fixture_resolve("K-jXhbt2gn4#libScePosix"),
        (MutexCall)fixture_resolve("2Z+PpY6CaJg#libScePosix"),
        (MutexCall)fixture_resolve("ltCfaGr2JGE#libScePosix"),
        16, 22
    };
    return (GuestApi){
        (AttrInit)fixture_resolve("F8bUHwAG284#libkernel"),
        (AttrType)fixture_resolve("iMp8QpE+XO4#libkernel"),
        (AttrDestroy)fixture_resolve("smWEktiyyG0#libkernel"),
        (SceInit)fixture_resolve("cmo1RIYva9o#libkernel"), NULL,
        (MutexCall)fixture_resolve("9UK1vLZQft4#libkernel"),
        (MutexCall)fixture_resolve("upoVrzMHFeE#libkernel"),
        (MutexCall)fixture_resolve("tn3VlD0hG60#libkernel"),
        (MutexCall)fixture_resolve("2Of0f+3mhhE#libkernel"),
        (int32_t)UINT32_C(0x80020010), (int32_t)UINT32_C(0x80020016)
    };
}

static void fixture_guest_flag_write(GuestMutex *mutex) {
    Allocation *allocation = fixture_find(mutex);
    if (allocation->size < GUEST_VIEW_BYTES + sizeof(pthread_mutex_t)) {
        fprintf(stderr, "mutex layout: allocation is only %zu bytes; guest view requires 0x100 bytes before the native mutex\n",
                allocation->size);
        exit(1);
    }
    fixture_check_guards(allocation);
    for (size_t i = 0; i < GUEST_VIEW_BYTES; ++i) CHECK(allocation->payload[i] == 0);
    uint32_t flags;
    memcpy(&flags, allocation->payload + GUEST_FLAG_OFFSET, sizeof(flags));
    flags |= 0x100;
    memcpy(allocation->payload + GUEST_FLAG_OFFSET, &flags, sizeof(flags));
}

static void fixture_check_guest_view(GuestMutex *mutex) {
    const Allocation *allocation = fixture_find(mutex);
    unsigned char expected[GUEST_VIEW_BYTES] = {0};
    const uint32_t flags = 0x100;
    memcpy(expected + GUEST_FLAG_OFFSET, &flags, sizeof(flags));
    CHECK(memcmp(allocation->payload, expected, sizeof(expected)) == 0);
    fixture_check_guards(allocation);
}

static void fixture_destroy(const GuestApi *api, GuestMutex **mutex) {
    GuestMutex *copied = *mutex;
    CHECK(api->destroy(mutex) == 0);
    CHECK((uintptr_t)*mutex == 2);
    CHECK(api->lock(mutex) == api->invalid);
    CHECK(api->trylock(mutex) == api->invalid);
    CHECK(api->unlock(mutex) == api->invalid);
    CHECK(api->destroy(mutex) == api->invalid);
    /* Preserve channel A's existing retained LIVE/DEAD behavior for copied handles. */
    CHECK(api->lock(&copied) == api->invalid);
    CHECK(api->trylock(&copied) == api->invalid);
    CHECK(api->unlock(&copied) == api->invalid);
    CHECK(api->destroy(&copied) == api->invalid);
    fixture_check_guest_view(copied);
}

static void fixture_explicit(const GuestApi *api, int type) {
    GuestAttr *attr = NULL;
    GuestMutex *mutex = NULL;
    CHECK(api->attr_init(&attr) == 0);
    CHECK(api->attr_type(&attr, type) == 0);
    CHECK((api->sce_init ? api->sce_init(&mutex, &attr, "layout") : api->posix_init(&mutex, &attr)) == 0);
    CHECK(api->attr_destroy(&attr) == 0);
    fixture_guest_flag_write(mutex);
    CHECK(api->lock(&mutex) == 0);
    CHECK(api->trylock(&mutex) == (type == 2 ? 0 : api->busy));
    CHECK(api->destroy(&mutex) == api->busy);
    if (type == 2) CHECK(api->unlock(&mutex) == 0);
    CHECK(api->unlock(&mutex) == 0);
    CHECK(api->trylock(&mutex) == 0);
    CHECK(api->unlock(&mutex) == 0);
    fixture_check_guest_view(mutex);
    fixture_destroy(api, &mutex);
}

static void fixture_static(const GuestApi *api, uintptr_t initial) {
    GuestMutex *mutex = (GuestMutex *)initial;
    CHECK(api->lock(&mutex) == 0);
    fixture_guest_flag_write(mutex);
    CHECK(api->trylock(&mutex) == api->busy);
    CHECK(api->unlock(&mutex) == 0);
    CHECK(api->trylock(&mutex) == 0);
    CHECK(api->unlock(&mutex) == 0);
    fixture_destroy(api, &mutex);
}

int main(void) {
    for (int posix = 0; posix <= 1; ++posix) {
        const GuestApi api = fixture_api(posix);
        for (int type = 1; type <= 4; ++type) fixture_explicit(&api, type);
        fixture_static(&api, 0);
        fixture_static(&api, 1);
    }
    for (size_t i = 0; i < fixture_allocation_count; ++i) {
        if (fixture_allocations[i].raw) {
            fixture_check_guards(&fixture_allocations[i]);
            free(fixture_allocations[i].raw);
        }
    }
    puts("mutex layout: guest flag writes, SCE/POSIX calls, static and copied-dead handles passed");
    return 0;
}
