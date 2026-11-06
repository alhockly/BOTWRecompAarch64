// Linux and Android: the game's command lists translated on a render thread of their own (LARECOMP_VK_ASYNC=0: off).
//
// The game's thread records its commands into its own command memory (cmdbuf.c) and, at submit, used to translate
// them to Vulkan right there: 3.5-5.5 ms of each frame, a third of a 60 fps frame on top of the game's own work. Here
// the game's thread only queues the request and goes on; the render thread runs the same translation in the same order.
// That is the contract a real GPU has too: the game reuses command memory, and rewrites what the GPU reads, only after
// waiting on a fence - so the fences cover the translation: a fence gets a logical number when it is queued (or when
// its command list is), and a wait first waits for the render thread to reach it, then for the GPU as before.
//
// Requests: EXEC (a command list), PRESENT (the window's half of backend_present: the game's half - pacing, input,
// tests - stays on the game's thread), FENCE (nvnQueueFenceSync). Everything else that reaches into the renderer from
// the game's thread (a texture destroyed, a movie frame, the heap, a reset) first waits for the queue to empty
// (vk_async_drain): those are rare, and so they run as before, with the render thread idle.
#if defined(__linux__)
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../nvn.h"
#include "../../platform.h"
#include "../../rt.h"

enum { REQ_EXEC, REQ_PRESENT, REQ_FENCE };
typedef struct { int type; uint64_t a; void *p; uint64_t id; } Req;

#define QN 4096  // requests in flight (a frame has a few dozen)
static Req q[QN];
static uint64_t q_head, q_tail;   // q_tail: next id to queue (ids start at 1), q_head: next to run
static uint64_t q_done;           // the last id the render thread finished
static int q_presents;            // PRESENT requests queued and not finished (the game's lead)
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER, q_less = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t p_lock = PTHREAD_MUTEX_INITIALIZER;  // producers: an id, its list's fences and its push together
pthread_t g_vk_render_thread;  // (also read by device.c renderer_present_count, many times a draw)
bool g_vk_render_started;
// What the queue thread is inside, for a wait that has already lasted seconds (the game thread logs it).
static int busy_kind = -1;
static uint64_t busy_id;
static int on = -1;

// The game's thread may run this many frames ahead of the render thread (the translation of one frame overlaps the
// game's work on the next; more only adds latency). LARECOMP_VK_ASYNC_AHEAD=n.
static int ahead = 1;

// The render thread's work for its last frame (requests run between two presents, ns): the frame pacer's other half
static uint64_t frame_work, last_frame_work, frame_cpu, last_frame_cpu;
uint64_t vk_async_frame_work_ns(void) { return __atomic_load_n(&last_frame_work, __ATOMIC_RELAXED); }
uint64_t vk_async_frame_cpu_ns(void) { return __atomic_load_n(&last_frame_cpu, __ATOMIC_RELAXED); }

#ifdef __ANDROID__
void kernel_blocking_begin(void);        // kernel.c: core 0 to the game's other threads while a guest thread waits here
void kernel_blocking_end(void);
#endif
void vk_execute_now(uint64_t handle);    // exec.c: translate and submit a command list
void vk_present_now(NvnTexture *t, uint64_t asked);  // device.c: the window's half of backend_present
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
uint64_t renderer_fence_seq(void);

bool vk_async_on(void) {
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_ASYNC");  // (on by default: =0 turns it off)
        on = !e || atoi(e) != 0;
        const char *a = getenv("LARECOMP_VK_ASYNC_AHEAD");
        if (a && atoi(a) > 0) ahead = atoi(a);
        if (on) LOG("[vk] async: command lists translated on %s (%d frame%s ahead)\n",
#ifdef __ANDROID__
                    "a render thread",
#else
                    "the window thread",
#endif
                    ahead, ahead > 1 ? "s" : "");
    }
    return on;
}

bool vk_async_is_render_thread(void) { return g_vk_render_started && pthread_equal(pthread_self(), g_vk_render_thread); }

// Thread CPU time excludes GPU fences, swapchain acquisition and time descheduled by Android.
static uint64_t render_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *busy_what(int kind) {
    return kind == REQ_EXEC ? "a command list" : kind == REQ_PRESENT ? "a present" : kind == REQ_FENCE ? "a fence" : "nothing";
}

static bool room_for(int type) {  // q_lock held
    return q_tail - q_head < QN && !(type == REQ_PRESENT && q_presents >= ahead);
}

// q_lock held on entry and on return. A wait of several seconds is logged: the window stays up and the audio
// thread keeps going, so a stuck queue otherwise looks like a freeze with nothing in the log.
static void wait_room(int type) {
    while (!room_for(type)) {
        int kind = __atomic_load_n(&busy_kind, __ATOMIC_ACQUIRE);
        uint64_t id = __atomic_load_n(&busy_id, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&q_lock);
#ifdef __ANDROID__
        kernel_blocking_begin();
#endif
        pthread_mutex_lock(&q_lock);
        while (!room_for(type)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            if (pthread_cond_timedwait(&q_less, &q_lock, &ts) == ETIMEDOUT && !room_for(type)) {
                kind = __atomic_load_n(&busy_kind, __ATOMIC_ACQUIRE);
                id = __atomic_load_n(&busy_id, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&q_lock);
                LOG("[vk] async: still waiting for the queue thread after 5 s (it is in %s %llu)\n", busy_what(kind),
                    (unsigned long long)id);
                fflush(stderr);
                pthread_mutex_lock(&q_lock);
            }
        }
        pthread_mutex_unlock(&q_lock);
#ifdef __ANDROID__
        kernel_blocking_end();
#endif
        pthread_mutex_lock(&q_lock);
    }
}

static void wait_done(uint64_t id) {  // q_lock held on entry and on return
    while (q_done < id) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        if (pthread_cond_timedwait(&q_less, &q_lock, &ts) == ETIMEDOUT && q_done < id) {
            int kind = __atomic_load_n(&busy_kind, __ATOMIC_ACQUIRE);
            uint64_t bid = __atomic_load_n(&busy_id, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&q_lock);
            LOG("[vk] async: still waiting for request %llu after 5 s (queue thread is in %s %llu)\n",
                (unsigned long long)id, busy_what(kind), (unsigned long long)bid);
            fflush(stderr);
            pthread_mutex_lock(&q_lock);
        }
    }
}

#ifndef __ANDROID__
void vk_async_wake_window(void);  // device.c: SDL_PushEvent, so the window thread leaves its wait
#endif

// One queued request. q_lock is not held. The request stays in the ring until the caller advances q_head.
static bool run_request(Req r) {
    __atomic_store_n(&busy_id, r.id, __ATOMIC_RELAXED);
    __atomic_store_n(&busy_kind, r.type, __ATOMIC_RELEASE);
    // Keep the established ADPF signal by default. CPU-only timing is an explicit experiment:
    // excluding descheduling/waits can understate the work seen by Android's frequency governor.
    const char *timing = getenv("LARECOMP_ADPF_CPU_TIME");
    bool cpu_timing = timing && atoi(timing) != 0;
    uint64_t t0 = plat_now_ns(), cpu0 = cpu_timing ? render_cpu_ns() : 0;
#ifdef __ANDROID__
    extern void vk_tl(const char *what, uint64_t a, uint64_t b);  // (exec.c: LARECOMP_FRAME_TIMELINE)
    vk_tl(r.type == REQ_EXEC ? "render: list start" : r.type == REQ_PRESENT ? "render: present start" : "render: fence", r.id, 0);
#endif
    if (r.type == REQ_EXEC) { vk_execute_now(r.a); free(r.p); }  // (r.p: the list copy, vk_async_execute)
    else if (r.type == REQ_PRESENT) vk_present_now((NvnTexture *)r.p, r.a);
    else if (r.type == REQ_FENCE) nvn_sync_set_seq(r.a, renderer_fence_seq());
#ifdef __ANDROID__
    if (r.type != REQ_FENCE) vk_tl(r.type == REQ_EXEC ? "render: list end" : "render: present end", r.id, 0);
#endif
    uint64_t elapsed = plat_now_ns() - t0;
    frame_cpu += cpu_timing ? render_cpu_ns() - cpu0 : elapsed;
    frame_work += elapsed;
    bool present = r.type == REQ_PRESENT;
    if (present) {
        __atomic_store_n(&last_frame_work, frame_work, __ATOMIC_RELAXED);
        __atomic_store_n(&last_frame_cpu, frame_cpu, __ATOMIC_RELAXED);
#ifdef __ANDROID__
        extern void plat_render_work(uint64_t work_ns, uint64_t target_ns);
        plat_render_work(frame_cpu, 16666667ull);  // (ADPF: clocks for 60 fps on this thread too)
#endif
        frame_work = frame_cpu = 0;
    }
    __atomic_store_n(&busy_kind, -1, __ATOMIC_RELEASE);
    return present;
}

static void finish_request(Req r) {  // q_lock held
    q_head++;
    q_done = r.id;
    if (r.type == REQ_PRESENT) q_presents--;
    pthread_cond_broadcast(&q_less);
}

#ifdef __ANDROID__
static void *render_main(void *arg) {
    (void)arg;
    extern void plat_thread_unpin(void);
    plat_thread_unpin();  // (the presenting thread is pinned to the fast cores)
    pthread_mutex_lock(&q_lock);
    for (;;) {
        while (q_head == q_tail) pthread_cond_wait(&q_more, &q_lock);
        Req r = q[q_head % QN];
        pthread_mutex_unlock(&q_lock);
        run_request(r);
        pthread_mutex_lock(&q_lock);
        finish_request(r);
    }
    return NULL;
}
#else
// Desktop Linux: the window thread runs the queue (vk_async_service). A third thread presenting through Wayland
// while this thread blocks in SDL_WaitEvent deadlocks; the game then waits on the queue and frames stop, audio
// does not. One present per call, then back to the window loop, so input and close still run.
void vk_async_bind_service_thread(void) {
    if (!vk_async_on()) return;
    g_vk_render_thread = pthread_self();
    g_vk_render_started = true;
}

bool vk_async_pending(void) {
    if (!g_vk_render_started) return false;
    pthread_mutex_lock(&q_lock);
    bool pending = q_head != q_tail;
    pthread_mutex_unlock(&q_lock);
    return pending;
}

void vk_async_service(void) {
    if (!vk_async_on() || !g_vk_render_started || !pthread_equal(pthread_self(), g_vk_render_thread)) return;
    for (int n = 0; n < 64; n++) {
        pthread_mutex_lock(&q_lock);
        if (q_head == q_tail) { pthread_mutex_unlock(&q_lock); return; }
        Req r = q[q_head % QN];
        pthread_mutex_unlock(&q_lock);
        bool present = run_request(r);
        pthread_mutex_lock(&q_lock);
        finish_request(r);
        pthread_mutex_unlock(&q_lock);
        if (present) return;
    }
}
#endif

// Publish one request. `mark` runs with the id chosen and p_lock held, before the queue thread can see the
// request, and never while this thread waits: the wait drops p_lock (the queue thread, or another producer, may
// need it). The id is assigned only once there is room, still under p_lock, so two producers cannot take the same one.
static uint64_t publish(int type, uint64_t a, void *p, void (*mark)(uint64_t id, uint64_t handle)) {
#ifdef __ANDROID__
    pthread_mutex_lock(&q_lock);
    if (!g_vk_render_started) {
        g_vk_render_started = true;
        pthread_create(&g_vk_render_thread, NULL, render_main, NULL);
    }
    pthread_mutex_unlock(&q_lock);
#endif
    pthread_mutex_lock(&p_lock);
    for (;;) {
        pthread_mutex_lock(&q_lock);
        if (!room_for(type)) {
            pthread_mutex_unlock(&q_lock);
            pthread_mutex_unlock(&p_lock);
            pthread_mutex_lock(&q_lock);
            wait_room(type);
            pthread_mutex_unlock(&q_lock);
            pthread_mutex_lock(&p_lock);
            continue;
        }
        uint64_t id = q_tail + 1;  // (held with p_lock: no other producer changes q_tail)
        pthread_mutex_unlock(&q_lock);
        if (mark) mark(id, a);
        pthread_mutex_lock(&q_lock);
        id = ++q_tail;
#ifdef __ANDROID__
        extern void vk_tl(const char *what, uint64_t a, uint64_t b);
        vk_tl(type == REQ_EXEC ? "game: list sent" : type == REQ_PRESENT ? "game: present sent" : "game: fence sent", id, 0);
#endif
        q[(id - 1) % QN] = (Req){type, a, p, id};
        if (type == REQ_PRESENT) q_presents++;
        pthread_cond_signal(&q_more);
        pthread_mutex_unlock(&q_lock);
        pthread_mutex_unlock(&p_lock);
#ifndef __ANDROID__
        vk_async_wake_window();
#endif
        return id;
    }
}

// Until the render thread has finished request `id` (0: nothing to wait for).
void vk_async_wait_id(uint64_t id) {
    if (!id || !g_vk_render_started || vk_async_is_render_thread()) return;
    pthread_mutex_lock(&q_lock);
    wait_done(id);
    pthread_mutex_unlock(&q_lock);
}

// Until every request queued so far has run (the render thread is idle after it).
void vk_async_drain(void) {
    if (!g_vk_render_started || vk_async_is_render_thread()) return;
    pthread_mutex_lock(&q_lock);
    uint64_t id = q_tail;
    wait_done(id);
    pthread_mutex_unlock(&q_lock);
}

// The fences a command list signals (CMD_FENCE_SYNC, in it or in the lists it calls) get the list's request: a wait
// on one first waits for the render thread to have translated it (its GPU submission is only known then).
static void mark_fences(uint64_t p, uint64_t id, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) mark_fences(h[i], id, depth + 1);
        } else if (r->op == CMD_FENCE_SYNC && r->x[1]) {
            NvnSync *s = HOST(NvnSync, r->x[1]);
            if (!s) { s = calloc(1, sizeof *s); SET_HOST(r->x[1], s); }
            s->signaled = 1;
            s->lseq = id;
        }
        if (!r->size) return;
        p += r->size;
    }
}

// A copy of a submitted list, the lists it calls inlined: NVN reads the lists (in control memory) when they are
// submitted, so a game may record over them right after (BOTW does), while the render thread runs them later.
typedef struct { uint8_t *b; size_t n, cap; } Snap;
static void snap_add(Snap *s, uint64_t p, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (!r->size) return;
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) snap_add(s, h[i], depth + 1);
        } else {
            if (s->n + r->size > s->cap) {
                s->cap = (s->n + r->size) * 2 > 0x10000 ? (s->n + r->size) * 2 : 0x10000;
                s->b = realloc(s->b, s->cap);
                if (!s->b) FATAL("vk: out of memory for a command list copy\n");
            }
            memcpy(s->b + s->n, r, r->size);
            s->n += r->size;
        }
        p += r->size;
    }
}
static uint8_t *snap_list(uint64_t handle) {
    Snap s = {0};
    snap_add(&s, handle, 0);
    s.b = realloc(s.b, s.n + sizeof(CmdRec));
    if (!s.b) FATAL("vk: out of memory for a command list copy\n");
    memset(s.b + s.n, 0, sizeof(CmdRec));  // CMD_END
    ((CmdRec *)(s.b + s.n))->size = sizeof(CmdRec);
    return s.b;
}

static void mark_exec(uint64_t id, uint64_t handle) { mark_fences(handle, id, 0); }

void vk_async_execute(uint64_t handle) {
    uint8_t *copy = getenv("LARECOMP_NO_LIST_COPY") ? NULL : snap_list(handle);
    if (copy) handle = (uint64_t)(uintptr_t)copy;
    publish(REQ_EXEC, handle, copy, mark_exec);  // (the queue thread frees the copy once run)
}

void vk_async_present(NvnTexture *t) {
    publish(REQ_PRESENT, plat_now_ns(), t, NULL);  // (a: when the game handed it over: device.c vsync_update)
}

uint64_t vk_async_fence(uint64_t guest_sync) {
    return publish(REQ_FENCE, guest_sync, NULL, NULL);
}
#else
typedef int vk_async_unused;  // (Linux and Android only)
#endif
