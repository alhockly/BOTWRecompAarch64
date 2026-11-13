// nn::vfx::detail::BufferAllocator (1.9.0): Alloc, Free and the end-of-frame flush of deferred frees, under one lock.
// The allocator keeps its blocks in an intrusive list sorted by address and has no lock of its own: the game calls it
// from the main thread and from job workers on cores 1 and 2, which never overlap inside it on the Switch (a core's
// threads take turns). Here they run at once, and two of them linked one block into the list twice - the main thread
// then looped forever in the flush (RP5, 2026-11-13: frozen after 5-30 minutes; kernel.c's per-core turns cured it
// too, at a heavy cost in stutter). These are host versions of the three functions, entered through a jump written
// over the recompiled ones, so direct calls reach them too. The lock alone did not stop the corruption (it was never
// contended), so they also repair it: a broken list is rebuilt from the slots, a slot still linked is never reused, and a
// block freed twice is freed once - each logged with the caller. LARECOMP_VFX_LOCK=0: off.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#if defined(__linux__) && !defined(__ANDROID__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../hle.h"
#include "../kernel.h"

#define GA(a) ((uint64_t)(a) - 0x7000000000ull + GUEST_LO)

// BufferAllocator fields
enum {
    BA_BASE = 368, BA_TOTAL = 376, BA_HEAD = 0x180, BA_DEFERRED = 0x190, BA_SLOTS = 432, BA_USED = 440,
    BA_COUNT = 444, BA_BYTES = 448, BA_NEXT_SLOT = 456, BA_FREE_SLOT = 472, BA_CUR = 480,
};
// a slot (48 bytes): address, size, its link in the sorted list (+16), its link in a deferred list (+32)

static inline uint64_t *Q(uint64_t a) { return (uint64_t *)(uintptr_t)a; }
static inline uint32_t *W(uint64_t a) { return (uint32_t *)(uintptr_t)a; }

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_holder_what;
static uint64_t g_holder_lr, g_holder_tid, g_waits;

static uint64_t tid_of(Cpu *c) { return c->thread ? ((KThread *)c->thread)->id : 0; }

static void lock(Cpu *c, const char *what) {
    if (pthread_mutex_trylock(&g_lock)) {
        const char *w = g_holder_what;
        uint64_t lr = g_holder_lr, t = g_holder_tid;
        pthread_mutex_lock(&g_lock);
        if (g_waits++ < 20 || g_waits % 1000 == 0)
            LOG("[vfx] BufferAllocator %s (thread %llu, lr %llx) waited for %s (thread %llu, lr %llx): %llu so far\n", what,
                (unsigned long long)tid_of(c), (unsigned long long)c->x[30], w ? w : "?", (unsigned long long)t,
                (unsigned long long)lr, (unsigned long long)g_waits);
    }
    g_holder_what = what, g_holder_lr = c->x[30], g_holder_tid = tid_of(c);
}
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

static void note(Cpu *c, uint64_t o, const char *fmt, ...) {
    static int logged;
    if (logged++ >= 40) return;
    char what[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    LOG("[vfx] BufferAllocator %llx: %s (thread %llu, lr %llx)\n", (unsigned long long)o, what,
        (unsigned long long)tid_of(c), (unsigned long long)c->x[30]);
}

// A link that leads back to itself (not the head) or a walk longer than the allocator's slots: the list is broken.
static bool broken(uint64_t o, uint64_t n, uint64_t steps) {
    return Q(n)[1] == n || steps > (uint64_t)*W(o + BA_COUNT) + 2;
}

static inline bool self_linked(uint64_t l) { return Q(l)[0] == l && Q(l)[1] == l; }

// A slot Alloc may take: no block, and in neither list
static inline bool slot_clean(uint64_t s) { return !Q(s)[0] && self_linked(s + 16) && self_linked(s + 32); }

// The game's unlink: neighbours joined, the node linked to itself
static void unlink_node(uint64_t n) {
    uint64_t prev = Q(n)[0], next = Q(n)[1], np = Q(next)[0];
    Q(prev)[1] = next;
    Q(next)[0] = Q(n)[0];
    Q(np)[1] = n;
    Q(n)[0] = np;
}

// The game's insert of the self-linked node `l` before `at`
static void link_before(uint64_t l, uint64_t at) {
    uint64_t lp = Q(l)[0];
    Q(l)[0] = Q(at)[0];
    Q(lp)[1] = at;
    Q(Q(at)[0])[1] = l;
    Q(at)[0] = lp;
}

// The sorted list rebuilt from the slots (a slot with an address is a live block), with the count and bytes in use
static void rebuild(Cpu *c, uint64_t o, const char *where) {
    uint64_t head = o + BA_HEAD, slots = Q(o + BA_SLOTS)[0], bytes = 0;
    int32_t count = (int32_t)*W(o + BA_COUNT);
    uint32_t used = 0;
    Q(head)[0] = Q(head)[1] = head;
    for (int32_t k = 0; k < count; k++) {
        uint64_t l = slots + (uint64_t)k * 48 + 16;
        Q(l)[0] = Q(l)[1] = l;
    }
    for (int32_t k = 0; k < count; k++) {
        uint64_t s = slots + (uint64_t)k * 48, at = Q(head)[1];
        if (!Q(s)[0]) continue;
        while (at != head && Q(at - 16)[0] < Q(s)[0]) at = Q(at)[1];
        link_before(s + 16, at);
        used++, bytes += Q(s)[1];
    }
    note(c, o, "broken list in %s: rebuilt from the slots, %u blocks (was %u)", where, used, *W(o + BA_USED));
    *W(o + BA_USED) = used;
    Q(o + BA_BYTES)[0] = bytes;
    Q(o + BA_FREE_SLOT)[0] = 0;
}

// Both deferred lists emptied and every slot's deferred link reset (the pending frees are lost: a leak, not a hang)
static void reset_deferred(Cpu *c, uint64_t o) {
    uint64_t slots = Q(o + BA_SLOTS)[0];
    int32_t count = (int32_t)*W(o + BA_COUNT);
    for (int i = 0; i < 2; i++) {
        uint64_t dh = o + BA_DEFERRED + 16 * (uint64_t)i;
        Q(dh)[0] = Q(dh)[1] = dh;
    }
    for (int32_t k = 0; k < count; k++) {
        uint64_t l = slots + (uint64_t)k * 48 + 32;
        Q(l)[0] = Q(l)[1] = l;
    }
    note(c, o, "broken deferred list: both reset, their pending frees dropped");
}

// The node of the block at `addr` in the sorted list, or 0
static uint64_t find(Cpu *c, uint64_t o, uint64_t addr, const char *where) {
    for (int pass = 0; pass < 2; pass++) {
        uint64_t head = o + BA_HEAD, steps = 0;
        bool bad = false;
        for (uint64_t n = Q(head)[1]; n != head; n = Q(n)[1]) {
            if (Q(n - 16)[0] == addr) return n;
            if ((bad = broken(o, n, ++steps))) break;
        }
        if (!bad) return 0;
        rebuild(c, o, where);
    }
    return 0;
}

static void erase(Cpu *c, uint64_t o, uint64_t n, bool from_flush) {
    uint64_t slot = n - 16;
    if (!from_flush && !self_linked(slot + 32)) {  // freed now while a deferred free of it waits: that one dropped
        note(c, o, "block %llx freed while a deferred free of it is pending", (unsigned long long)Q(slot)[0]);
        unlink_node(slot + 32);
    }
    Q(slot)[0] = 0;
    Q(o + BA_BYTES)[0] -= Q(slot)[1];
    unlink_node(n);
    Q(o + BA_FREE_SLOT)[0] = slot;
    *W(o + BA_USED) -= 1;
}

// void *Alloc(size, alignment): 0 when no slot or no room
static void vfx_alloc(Cpu *c) {
    uint64_t o = c->x[0], size = c->x[1], align = c->x[2], r = 0;
    lock(c, "Alloc");
    int32_t count = (int32_t)*W(o + BA_COUNT);
    if (count <= (int32_t)*W(o + BA_USED)) goto done;
    uint64_t slots = Q(o + BA_SLOTS)[0], slot = Q(o + BA_FREE_SLOT)[0];
    if (slot && !slot_clean(slot)) {
        note(c, o, "the free-slot hint %llx is in use (block %llx, prev %llx)", (unsigned long long)slot,
             (unsigned long long)Q(slot)[0], (unsigned long long)Q(slot + 16)[0]);
        slot = 0;
    }
    if (!slot) {
        uint32_t i = *W(o + BA_NEXT_SLOT) + 1;
        if (i == (uint32_t)count) i = 0;
        *W(o + BA_NEXT_SLOT) = i;
        uint64_t s = slots + (uint64_t)(int32_t)i * 48;
        if (slot_clean(s)) slot = s;
        else
            for (int32_t k = 0; k < count; k++) {
                s = slots + (uint64_t)k * 48;
                if (slot_clean(s)) {
                    slot = s;
                    *W(o + BA_NEXT_SLOT) = (uint32_t)k;
                    break;
                }
                if (!Q(s)[0])  // (the game's own Alloc would have taken it, and linked it twice)
                    note(c, o, "slot %llx has no block but is still linked (sorted prev %llx, deferred prev %llx)",
                         (unsigned long long)s, (unsigned long long)Q(s + 16)[0], (unsigned long long)Q(s + 32)[0]);
            }
        if (!slot) goto done;
        Q(o + BA_FREE_SLOT)[0] = slot;
    }
    uint64_t head = o + BA_HEAD, base = Q(o + BA_BASE)[0], am = align - 1, an = -align, cur, at, addr;
    for (int pass = 0;; pass++) {
        bool bad = false;
        uint64_t steps = 0;
        cur = base, at = head, addr = 0;
        for (uint64_t n = Q(head)[1]; n != head; n = Q(n)[1]) {
            uint64_t b = Q(n - 16)[0];
            cur = (am + cur) & an;
            if ((int64_t)(b - cur) >= (int64_t)size) { at = n, addr = cur; break; }
            cur = b + Q(n - 16)[1];
            if ((bad = broken(o, n, ++steps))) break;
        }
        if (!bad) break;
        if (pass) goto done;
        rebuild(c, o, "Alloc");
    }
    if (at == head) {
        addr = (am + cur) & an;
        if ((int64_t)(base + Q(o + BA_TOTAL)[0] - addr) < (int64_t)size) goto done;
    }
    Q(slot)[0] = addr;
    Q(slot)[1] = size;
    link_before(slot + 16, at);
    *W(o + BA_USED) += 1;
    Q(o + BA_FREE_SLOT)[0] = 0;
    Q(o + BA_BYTES)[0] += size;
    r = addr;
done:
    unlock();
    c->x[0] = r;
}

// Free(addr, now): now, or queued for the flush two frames on
static void vfx_free(Cpu *c) {
    uint64_t o = c->x[0], addr = c->x[1];
    bool now = c->x[2] & 1;
    if (!addr) return;
    lock(c, "Free");
    uint64_t n = find(c, o, addr, "Free");
    if (!n) {
        static int missing;
        if (missing++ < 10) note(c, o, "free of %llx, not allocated", (unsigned long long)addr);
    } else if (now) {
        erase(c, o, n, false);
    } else if (!self_linked(n + 16)) {
        note(c, o, "block %llx freed twice before the flush", (unsigned long long)addr);
    } else {
        link_before(n + 16, o + BA_DEFERRED + 16 * (uint64_t)(int64_t)(int32_t)*W(o + BA_CUR));
    }
    unlock();
}

// The flush (once a frame): the other deferred list's blocks freed, that list emptied, and it becomes the current one
static void vfx_flush(Cpu *c) {
    uint64_t o = c->x[0];
    lock(c, "Flush");
    int32_t cur = 1 - (int32_t)*W(o + BA_CUR);
    *W(o + BA_CUR) = (uint32_t)cur;
    uint64_t dh = o + BA_DEFERRED + 16 * (uint64_t)(int64_t)cur, steps = 0;
    bool bad = false;
    for (uint64_t m = Q(dh)[1]; m != dh; m = Q(m)[1]) {
        if ((bad = broken(o, m, ++steps))) break;
        uint64_t n = find(c, o, Q(m - 32)[0], "Flush");
        if (n == m - 16) erase(c, o, n, true);
        else if (n) note(c, o, "deferred block %llx belongs to another slot (%llx, not %llx)",
                      (unsigned long long)Q(m - 32)[0], (unsigned long long)(n - 16), (unsigned long long)(m - 32));
    }
    if (bad) reset_deferred(c, o);
    else
        while (Q(dh)[1] != dh) unlink_node(Q(dh)[1]);
    unlock();
}

void vfx_alloc_install(void) {
#if defined(__linux__) && !defined(__ANDROID__) && defined(__aarch64__)
    const char *e = getenv("LARECOMP_VFX_LOCK");
    if (g_game_version != 190 || (e && !atoi(e))) return;
    static const struct { uint64_t addr; uint32_t first; HostFn fn; } fns[] = {
        {0x700004e780ull, 0xb941bc09u, vfx_alloc},  // ldr w9, [x0, #444]
        {0x700004e8f0ull, 0xb40004c1u, vfx_free},   // cbz x1, ...
        {0x700004e9c0ull, 0xb981e008u, vfx_flush},  // ldrsw x8, [x0, #480]
    };
    GuestFn at[3];
    for (int i = 0; i < 3; i++) {
        at[i] = dispatch_lookup(GA(fns[i].addr));
        if (*W(GA(fns[i].addr)) != fns[i].first || !at[i]) {
            LOG("[vfx] BufferAllocator lock: %llx is not the expected function, not installed\n",
                (unsigned long long)fns[i].addr);
            return;
        }
    }
    long pg = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < 3; i++) {
        uintptr_t p = (uintptr_t)at[i], page = p & ~(uintptr_t)(pg - 1), len = (p + 16 - page + pg - 1) & ~(uintptr_t)(pg - 1);
        uintptr_t to = (uintptr_t)fns[i].fn;
        uint32_t jump[4] = {0x58000050u, 0xd61f0200u, (uint32_t)to, (uint32_t)(to >> 32)};  // ldr x16, #8; br x16; .quad
        if (mprotect((void *)page, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
            LOG("[vfx] BufferAllocator lock: mprotect failed, not installed\n");
            return;
        }
        memcpy((void *)p, jump, sizeof jump);
        mprotect((void *)page, len, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)p, (char *)p + sizeof jump);
    }
    LOG("[vfx] BufferAllocator: Alloc, Free and Flush under one lock\n");
#endif
}
