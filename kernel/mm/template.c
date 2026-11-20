#include <kernel/proc.h>
#include <kernel/vm.h>
#include <kernel/arch.h>
#include <kernel/template.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/lifecycle_probe.h>
#include <asm/csr.h>

_Static_assert(NPROC <= 255 && HEAP_WINDOWS <= 16, "window accounting fields");

_Static_assert((USER_HEAP_END - 1) / SV39_LEAF_SPAN + 1 == HEAP_WINDOWS,
               "template directory must cover the heap");
#define WORDS_PER_WINDOW 8
#define CONFIRM_LIMIT 32
#define RELEASE_LIMIT 8

enum work_kind { CONFIRM, RELEASE, FINALIZE, WORK_KINDS };

struct template_job {
    struct proc *image;
    struct template_job *next[WORK_KINDS];
    unsigned policy, queued, remaining;
    bool closed;
    uint64 original[HEAP_WINDOWS][WORDS_PER_WINDOW];
    uint64 held[HEAP_WINDOWS][WORDS_PER_WINDOW];
    uint64 safe[HEAP_WINDOWS][WORDS_PER_WINDOW];
    uint8 dirty[HEAP_WINDOWS], blocked[HEAP_WINDOWS], ready[HEAP_WINDOWS];
    unsigned dirty_windows, ready_windows;
    unsigned scan_window, scan_word, scan_slot;
    uint64 intersection;
};

struct work_queue {
    struct template_job *head, *tail;
};

static struct work_queue work[WORK_KINDS];
#ifdef LIFECYCLE_PROBE
static unsigned pending;
#ifdef LIFECYCLE_COUNTS
static unsigned backlog;
#endif

unsigned template_work_pending(void)
{
    return (pending << 1) | (work[CONFIRM].head || work[RELEASE].head || work[FINALIZE].head);
}
#endif

struct branch_heap {
    struct proc *image;
    unsigned shared;
    bool mixed_heap;
    uint64 owned[HEAP_WINDOWS][WORDS_PER_WINDOW];
    uint64 dropped[HEAP_WINDOWS][WORDS_PER_WINDOW];
    uint16_t borrowed[HEAP_WINDOWS];
};
_Static_assert(sizeof(struct branch_heap) <= PAGE_SIZE, "branch metadata");
_Static_assert(sizeof(((struct branch_heap *)0)->dropped) == 576, "borrow metadata");
_Static_assert(sizeof(struct template_job) <= PAGE_SIZE, "template metadata");

/* USER_BASE guarantees both roots already have a private low middle table. */
static pte_t *directory(struct proc *p)
{
    return (pte_t *)sv39_pte_to_pa(p->pgtbl[0]);
}

static pte_t *table(struct proc *p, unsigned window)
{
    pte_t entry = directory(p)[window];
    return entry ? (pte_t *)sv39_pte_to_pa(entry) : NULL;
}

static unsigned first_slot(unsigned window)
{
    return window ? 0 : sv39_index(0, USER_HEAP_START);
}

int template_index_init(struct proc *image, unsigned policy)
{
    if (policy < TEMPLATE_WINDOW_BATCH) return 0;
    struct template_job *job = page_alloc(PAGE_ZONE_KERNEL);
    if (!job) return -12;
    image->reclaim_job = job;
    job->image = image;
    job->policy = policy;
    return 0;
}

/* The clone's mapped spans exclude holes and both fixed-image boundaries. */
void template_index_span(struct proc *image, uintptr_t va, uintptr_t end)
{
    struct template_job *job = image->reclaim_job;
    while (va < end) {
        unsigned w = va / SV39_LEAF_SPAN, i = sv39_index(0, va);
        unsigned offset = i % 64, word = i / 64;
        unsigned count = 64 - offset;
        if (count > (end - va) / PAGE_SIZE)
            count = (end - va) / PAGE_SIZE;
        uint64 bits = (UINT64_MAX >> (64 - count)) << offset;
        job->original[w][word] |= bits;
        job->held[w][word] |= bits;
        job->blocked[w] |= 1U << word;
        job->remaining += count;
        va += count * PAGE_SIZE;
    }
}

bool template_index_drained(const struct proc *image)
{
    const struct template_job *job = image->reclaim_job;
    return job && job->closed && !job->remaining;
}

void template_index_destroy(struct proc *image)
{
    struct template_job *job = image->reclaim_job;
    if (!job) return;
    assert(!job->closed || (!job->queued && !job->remaining),
           "live reclaim job destroyed");
    page_free(job);
}

static void enqueue(struct template_job *job, enum work_kind kind)
{
    if (job->queued & (1U << kind)) return;
    struct work_queue *q = &work[kind];
    if (q->tail) q->tail->next[kind] = job;
    else q->head = job;
    q->tail = job;
    job->queued |= 1U << kind;
}

static struct template_job *dequeue(enum work_kind kind)
{
    struct work_queue *q = &work[kind];
    struct template_job *job = q->head;
    q->head = job->next[kind];
    if (!q->head) q->tail = NULL;
    job->next[kind] = NULL;
    job->queued &= ~(1U << kind);
    return job;
}

static void dirty_words(struct template_job *job, unsigned w, unsigned words)
{
    if (!words) return;
    job->dirty[w] |= words;
    job->dirty_windows |= 1U << w;
}

static bool record_only(const struct template_job *job)
{
#ifdef LIFECYCLE_CHECKS
    return job->policy == TEMPLATE_RECORD_ONLY && job->image->template_branches;
#else
    (void)job;
    return false;
#endif
}

static void queue_work(struct template_job *job)
{
    if (job->ready_windows && !record_only(job))
        enqueue(job, RELEASE);
    if (job->dirty_windows || job->scan_slot)
        enqueue(job, CONFIRM);
    if (!job->remaining && !job->image->template_branches && !job->queued)
        enqueue(job, FINALIZE);
}

static void allow_window(struct template_job *job, unsigned w)
{
    job->ready[w] = job->blocked[w];
    job->blocked[w] = 0;
    if (job->ready[w])
        job->ready_windows |= 1U << w;
}

void template_close_job(struct proc *image)
{
    struct template_job *job = image->reclaim_job;
    assert(image->template_refs == 1 + image->template_branches, "template close refs");
    ++image->template_refs;
    job->closed = true;
#ifdef LIFECYCLE_CHECKS
    if (lifecycle_active) {
        for (unsigned i = 0; i < NPROC; ++i) {
            const struct proc *p = proc_slot(i);
            if (p->state == PROC_ZOMBIE && p->branch && p->branch->image == image)
                ++reclaim_stats.closed_zombies;
        }
    }
#endif
#ifdef LIFECYCLE_PROBE
    ++pending;
#endif
#ifdef LIFECYCLE_COUNTS
    backlog += job->remaining;
    if (lifecycle_active) reclaim_stats.pending = pending;
    reclaim_count(backlog, job->remaining);
    reclaim_max(max_pending, pending);
    reclaim_max(peak_backlog, backlog);
#endif
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
        if (job->policy != TEMPLATE_WINDOW_BATCH)
            dirty_words(job, w, job->blocked[w]);
        else if (!image->template_borrowers[w])
            allow_window(job, w);
    }
    queue_work(job);
}

static void confirm_word(struct template_job *job)
{
    bool starting = !job->scan_slot;
    if (starting) {
        unsigned w = __builtin_ctz(job->dirty_windows);
        unsigned word = __builtin_ctz(job->dirty[w]);
        job->dirty[w] &= ~(1U << word);
        if (!job->dirty[w]) job->dirty_windows &= ~(1U << w);
        job->scan_window = w;
        job->scan_word = word;
        job->intersection = job->held[w][word];
        job->scan_slot = 1;
    }

    unsigned w = job->scan_window, word = job->scan_word;
    if (!job->image->template_branches) {
        job->intersection = job->held[w][word];
    } else if (starting) {
        if (!job->intersection) job->scan_slot = 0;
        return;
    } else {
        const struct branch_heap *b = proc_slot(job->scan_slot - 1)->branch;
        if (b && b->image == job->image) {
            reclaim_count(slots_matching, 1);
            job->intersection &= b->dropped[w][word];
        } else {
            reclaim_count(slots_other, 1);
        }
        if (job->scan_slot++ != NPROC) return;
    }

    job->scan_slot = 0;
    uint64 safe = job->intersection & job->held[w][word];
    job->safe[w][word] = safe;
    if (safe == job->held[w][word])
        job->blocked[w] &= ~(1U << word);
    if (safe) job->ready[w] |= 1U << word;
    if (job->ready[w] && (job->policy != TEMPLATE_WINDOW_INCREMENTAL || !job->blocked[w]))
        job->ready_windows |= 1U << w;
}

static unsigned release_pages(struct template_job *job, unsigned budget)
{
    unsigned w = __builtin_ctz(job->ready_windows);
    unsigned word = __builtin_ctz(job->ready[w]);
    uint64 *held = &job->held[w][word];
    uint64 allowed = job->policy == TEMPLATE_WINDOW_BATCH ? *held : job->safe[w][word];
    pte_t *leaf = table(job->image, w) + 64 * word;
    void *frames[RELEASE_LIMIT];
    unsigned n = 0;
    assert(allowed && !(allowed & ~*held), "template release candidate");
    for (; allowed && n < budget; ++n) {
        unsigned i = __builtin_ctzl(allowed);
        uint64 bit = 1UL << i;
        assert(leaf[i] & PTE_V, "template held leaf missing");
        frames[n] = (void *)sv39_pte_to_pa(leaf[i]);
        leaf[i] = 0;
        *held &= ~bit;
        job->safe[w][word] &= ~bit;
        allowed &= ~bit;
    }
    if (!allowed) {
        job->ready[w] &= ~(1U << word);
        if (!job->ready[w]) job->ready_windows &= ~(1U << w);
    }
    /* A shared leaf can only be cleared after every borrow was revoked and
     * fenced. Page table storage is kept until the last branch unlinks it. */
    sfence_vma();
#ifdef LIFECYCLE_PROBE
    if (lifecycle_reclaim_recording()) {
        for (unsigned i = 0; i < n; ++i)
            lifecycle_original_watch((uintptr_t)frames[i], ORIGINAL_DROP);
    }
#endif
    size_t returned = page_release_user_batch(frames, n);
    reclaim_count(drops, n);
    reclaim_count(returned, returned);
    template_count(dropped_refs, n);
    job->remaining -= n;
#ifdef LIFECYCLE_COUNTS
    backlog -= n;
    if (lifecycle_active) reclaim_stats.backlog = backlog;
#endif
    return n;
}

static void finalize_job(void)
{
    struct template_job *job = dequeue(FINALIZE);
    struct proc *image = job->image;
    assert(image->template_refs == 1 && !image->template_branches &&
           !job->remaining && !job->queued, "template final job ref");
    template_put(image);
#ifdef LIFECYCLE_PROBE
    --pending;
    if (lifecycle_active) reclaim_stats.pending = pending;
#endif
}

bool template_reclaim_step(bool idle)
{
    (void)idle;
    if (!work[CONFIRM].head && !work[RELEASE].head && !work[FINALIZE].head) return false;
    assert(arch_is_boot_cpu() && !arch_irq_enabled() && kernel_vm_active() &&
           !proc_current(), "template reclaim context");
#ifdef LIFECYCLE_PROBE
    bool tracing = lifecycle_reclaim_recording();
    uint64 start = tracing ? lifecycle_time() : 0;
    unsigned long finalized = reclaim_stats.finalized;
#endif
    unsigned confirmed = 0, dropped = 0;
    for (; confirmed < CONFIRM_LIMIT && work[CONFIRM].head; ++confirmed) {
        struct template_job *job = dequeue(CONFIRM);
        if (work[CONFIRM].head) reclaim_count(rotation, 1);
        confirm_word(job);
        queue_work(job);
    }
    while (dropped < RELEASE_LIMIT && work[RELEASE].head) {
        struct template_job *job = dequeue(RELEASE);
        /* Give each waiting template a turn before a second release. */
        unsigned turn = work[RELEASE].head ? 1 : RELEASE_LIMIT - dropped;
        if (work[RELEASE].head) reclaim_count(rotation, 1);
        dropped += release_pages(job, turn);
        queue_work(job);
    }
    /* The final table/PCB teardown is accounted separately, at most once. */
    if (work[FINALIZE].head) finalize_job();
    reclaim_count(batches, 1);
    reclaim_count(busy, !idle);
    reclaim_count(idle, idle);
    reclaim_count(confirm_total, confirmed);
    reclaim_max(confirm_max, confirmed);
    reclaim_max(release_max, dropped);
#ifdef LIFECYCLE_PROBE
    if (tracing) {
        uint64 ticks = lifecycle_time() - start;
        reclaim_stats.batch_total += ticks;
        if (ticks > reclaim_stats.batch_max)
            reclaim_stats.batch_max = ticks;
        lifecycle_reclaim_trace(ticks, confirmed, dropped, idle,
                                reclaim_stats.finalized - finalized);
    }
#endif
    return true;
}

#ifdef LIFECYCLE_CHECKS
/* Explicit probe in process context. Record mode keeps its original leaves
 * pinned, so the checker can use their actual PA without consulting D. */
long template_audit(unsigned pid)
{
    for (unsigned slot = 0; slot < NPROC; ++slot) {
        const struct branch_heap *b = proc_slot(slot)->branch;
        if (!b || (unsigned)proc_slot(slot)->pid != pid) continue;
        struct template_job *job = b->image->reclaim_job;
        if (!job || !record_only(job) || !job->closed) return -22;
        if (job->dirty_windows || job->scan_slot) return -11;
        unsigned n = 0;
        for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
            for (unsigned word = 0; word < WORDS_PER_WINDOW; ++word) {
                uint64 original = job->original[w][word];
                while (original) {
                    unsigned bit = __builtin_ctzl(original);
                    uint64 mask = 1UL << bit;
                    unsigned i = word * 64 + bit;
                    uintptr_t va = w * SV39_LEAF_SPAN + i * PAGE_SIZE;
                    pte_t entry = table(job->image, w)[i];
                    assert(entry & PTE_V, "audit original held");
                    uintptr_t frame = sv39_pte_to_pa(entry);
                    bool borrowed = false;
                    for (unsigned who = 0; who < NPROC; ++who) {
                        const struct proc *p = proc_slot(who);
                        if (!p->branch || p->branch->image != job->image) continue;
                        uintptr_t pa;
                        uint64 flags;
                        bool live = sv39_lookup(p->pgtbl, va, &pa, &flags) &&
                                    (flags & PTE_U) && pa == frame;
                        assert(live != !!(p->branch->dropped[w][word] & mask),
                               "borrow bitmap disagrees with branch PTE");
                        borrowed |= live;
                    }
                    assert(borrowed != !!(job->safe[w][word] & mask),
                           "reclaim disagrees with actual branch PTEs");
                    n += !borrowed;
                    original &= ~mask;
                }
            }
        }
        return n;
    }
    return -22;
}
#endif

static void batch_refs(void **frames, unsigned count, bool retain)
{
    if (retain) {
        page_retain_user_batch(frames, count);
        template_count(retains, count);
        reclaim_count(retains, count);
        reclaim_count(retain_locks, 1);
    } else {
        size_t last = page_release_user_batch(frames, count);
        reclaim_count(ref_drops, count);
        reclaim_count(drop_locks, 1 + !!last);
    }
}

static void leaf_refs(pte_t *leaf, unsigned first, bool retain, bool batch)
{
    void *frames[PAGE_USER_REF_BATCH_MAX];
    unsigned used = 0;
    for (unsigned i = first; i < 512; ++i) {
        if (!retain) template_count(exit_slots, 1);
        if (!(leaf[i] & PTE_V)) continue;
        void *frame = (void *)sv39_pte_to_pa(leaf[i]);
        if (batch) {
            frames[used++] = frame;
            if (used < PAGE_USER_REF_BATCH_MAX) continue;
        } else {
            if (retain) {
                page_retain_user(frame);
                template_count(retains, 1);
                reclaim_count(retains, 1);
                reclaim_count(retain_locks, 1);
            } else {
                bool last = page_free(frame);
                reclaim_count(ref_drops, 1);
                reclaim_count(drop_locks, 1 + last);
            }
            continue;
        }
        batch_refs(frames, used, retain);
        used = 0;
    }
    if (used) batch_refs(frames, used, retain);
}

static void take_refs(struct proc *p, unsigned w)
{
    if (p->branch->image->template_owned) return;
    leaf_refs(table(p, w), first_slot(w), true, p->branch->image->template_ref_batch);
}

int branch_attach(struct proc *p, struct proc *image)
{
    struct branch_heap *b = page_alloc(PAGE_ZONE_KERNEL);
    if (!b) return -12;
    p->branch = b;
    b->image = image;
    ++image->template_refs;
    ++image->template_branches;
    unsigned first = first_slot(0);
    b->mixed_heap = image->mixed_heap;
    if (b->mixed_heap) {
        memcpy(table(p, 0) + first, table(image, 0) + first,
               (512 - first) * sizeof(pte_t));
        template_count(copied_slots, 512 - first);
        take_refs(p, 0);
    }
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        if (directory(image)[w]) {
            directory(p)[w] = directory(image)[w];
            b->shared |= 1U << w;
        }
    }
    return 0;
}

void branch_cancel(struct split_plan *plan)
{
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w)
        if (plan->fresh[w]) page_free(plan->fresh[w]);
}

int branch_prepare(struct proc *p, uintptr_t begin, uintptr_t end, struct split_plan *plan)
{
    if (!p->branch || begin >= USER_HEAP_END || end <= USER_HEAP_START) return 0;
    if (end > USER_HEAP_END) end = USER_HEAP_END;
    for (unsigned w = begin / SV39_LEAF_SPAN; w <= (end-1) / SV39_LEAF_SPAN; ++w) {
        if (!(p->branch->shared & (1U << w))) continue;
        plan->fresh[w] = page_alloc(PAGE_ZONE_KERNEL);
        if (!plan->fresh[w]) {
            branch_cancel(plan);
            return -12;
        }
        memcpy(plan->fresh[w], table(p,w), PAGE_SIZE);
    }
    return 0;
}

void branch_commit(struct proc *p, struct split_plan *plan)
{
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        if (!plan->fresh[w]) continue;
        directory(p)[w] = sv39_pa_to_pte((uintptr_t)plan->fresh[w]) | PTE_V;
        p->branch->shared &= ~(1U << w);
        take_refs(p, w);
        template_count(copied_slots, 512);
    }
}

void branch_undo(struct proc *p, struct split_plan *plan)
{
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        pte_t *leaf = plan->fresh[w];
        if (!leaf) continue;
        directory(p)[w] = directory(p->branch->image)[w];
        p->branch->shared |= 1U << w;
        sfence_vma();
        if (!p->branch->image->template_owned)
            leaf_refs(leaf, 0, false, p->branch->image->template_ref_batch);
        page_free(leaf);
    }
}

bool branch_borrows(struct proc *p, uintptr_t va)
{
    if (!p->branch || va < USER_HEAP_START || va >= USER_HEAP_END) return false;
    unsigned w = va / SV39_LEAF_SPAN, i = sv39_index(0, va);
    if (p->branch->shared & (1U << w)) return true;
    return p->branch->image->template_owned &&
           !(p->branch->owned[w][i/64] & (1UL << (i%64)));
}

/* Called at close, while the owner serializes mapping changes and branch teardown.
 * An original heap frame is still borrowed only if its branch slot is valid and
 * has not been replaced by a private mapping. Holes need no history. */
bool branch_count_borrows(struct proc *p, struct proc *image)
{
    struct branch_heap *b = p->branch;
    if (!b || b->image != image) return false;
    template_count(close_scan_branches, 1);
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
        if (!w && !b->mixed_heap) continue;
        pte_t *leaf = table(p, w);
        if (!leaf) continue;
        unsigned count = 0, first = first_slot(w);
        for (unsigned i = first; i < 512; ++i)
            if ((leaf[i] & PTE_V) && !(b->owned[w][i/64] & (1UL << (i%64))))
                ++count;
        template_count(close_scan_slots, 512 - first);
        b->borrowed[w] = count;
        if (count)
            ++image->template_borrowers[w];
    }
    return true;
}

/* The image root is never active. A branch removes its old translation and
 * fences before reporting its last borrow. Keep the directory links and page
 * table storage until the existing template reference count reaches zero. */
static void reclaim_window(struct proc *image, unsigned w)
{
    assert(image->template_tracking && !image->template_borrowers[w] &&
           !(image->template_reclaimed & (1U << w)), "template window reclaim");
    pte_t *leaf = table(image, w);
    assert(leaf, "template reclaim leaf");
    uint64 start = lifecycle_begin();
    for (unsigned i = first_slot(w); i < 512; ++i) {
        pte_t entry = leaf[i];
        if (entry & PTE_V) {
            leaf[i] = 0;
            uintptr_t frame = sv39_pte_to_pa(entry);
            lifecycle_original_watch(frame, ORIGINAL_DROP);
            bool returned = page_free((void *)frame);
            reclaim_count(drops, 1);
            reclaim_count(returned, returned);
            template_count(dropped_refs, 1);
        }
    }
    image->template_reclaimed |= 1U << w;
    template_count(released_windows, 1);
#ifdef LIFECYCLE_PROBE
    if (lifecycle_active) {
        uint64 ticks = lifecycle_time() - start;
        template_stats.release_ticks += ticks;
        if (ticks > template_stats.max_release_ticks)
            template_stats.max_release_ticks = ticks;
    }
#else
    (void)start;
#endif
}

void template_reclaim_unborrowed(struct proc *image)
{
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w)
        if (!image->template_borrowers[w] && (w || image->mixed_heap) && table(image, w))
            reclaim_window(image, w);
}

static void end_window_borrow(struct branch_heap *b, unsigned w)
{
    struct proc *image = b->image;
    b->borrowed[w] = 0;
    assert(image->template_borrowers[w], "template window borrow underflow");
    if (--image->template_borrowers[w]) return;
    struct template_job *job = image->reclaim_job;
    if (!job) {
        reclaim_window(image, w);
    } else {
        allow_window(job, w);
        queue_work(job);
    }
}

void branch_drop_borrow(struct proc *p, uintptr_t va)
{
    struct branch_heap *b = p->branch;
    struct template_job *job = b->image->reclaim_job;
    unsigned w = va / SV39_LEAF_SPAN;
#ifdef LIFECYCLE_PROBE
    if (lifecycle_reclaim_recording() && table(b->image, w)) {
        pte_t original = table(b->image, w)[sv39_index(0, va)];
        if (original & PTE_V)
            lifecycle_original_watch(sv39_pte_to_pa(original), ORIGINAL_BORROW);
    }
#endif
    if (job && job->policy != TEMPLATE_WINDOW_BATCH) {
        unsigned i = sv39_index(0, va), word = i / 64;
        uint64 bit = 1UL << (i % 64);
        assert((job->original[w][word] & bit) && !(b->dropped[w][word] & bit),
               "branch original borrow");
        b->dropped[w][word] |= bit;
        if (job->closed) {
            if (job->scan_slot && job->scan_window == w && job->scan_word == word &&
                proc_slot(job->scan_slot - 1) > p)
                reclaim_count(redirtied, 1);
            dirty_words(job, w, job->blocked[w] & (1U << word));
            queue_work(job);
        }
    }
    if (!b->image->template_tracking) return;
    assert(b->borrowed[w], "branch window borrow underflow");
    if (b->borrowed[w] == 1)
        end_window_borrow(b, w);
    else
        --b->borrowed[w];
}

void branch_map(struct proc *p, uintptr_t va)
{
    if (p->branch && va >= USER_HEAP_START && va < SV39_LEAF_SPAN)
        p->branch->mixed_heap = true;
    if (p->branch && p->branch->image->template_owned &&
        va >= USER_HEAP_START && va < USER_HEAP_END) {
        unsigned i = sv39_index(0, va);
        p->branch->owned[va/SV39_LEAF_SPAN][i/64] |= 1UL << (i%64);
    }
}

void branch_unmap(struct proc *p, uintptr_t va, size_t pages)
{
    for (; pages; --pages, va += PAGE_SIZE) {
        unsigned w = va / SV39_LEAF_SPAN, i = sv39_index(0, va);
        pte_t *slot = &table(p,w)[i];
        void *frame = (void *)sv39_pte_to_pa(*slot);
        bool borrowed = branch_borrows(p, va);
        *slot = 0;
        sfence_vma();
        if (borrowed)
            branch_drop_borrow(p, va);
        else
            page_free(frame);
        p->branch->owned[w][i/64] &= ~(1UL << (i%64));
        sv39_prune_empty(p->pgtbl, va);
    }
}

void branch_detach(struct proc *p)
{
    struct branch_heap *b = p->branch;
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
        if (!w && !b->mixed_heap) continue;
        pte_t *leaf = table(p, w);
        if (!leaf) continue;
        pte_t link = directory(p)[w];
        directory(p)[w] = 0;
        /* In the mixed window revoke heap leaves before dropping any frame.
         * Both exec and wait run on the kernel root with user translation inactive. */
        sfence_vma();
        if (b->borrowed[w])
            end_window_borrow(b, w);
        if (b->shared & (1U << w)) continue;
        if (b->image->template_owned) {
            for (unsigned j = 0; j < 8; ++j) {
                template_count(bitmap_words, 1);
                uint64 bits = b->owned[w][j];
                for (unsigned i = 64*j; bits; bits >>= 1, ++i) {
                    if (!(bits & 1)) continue;
                    page_free((void *)sv39_pte_to_pa(leaf[i]));
                    template_count(private_drops, 1);
                }
            }
        } else {
            leaf_refs(leaf, first_slot(w), false, b->image->template_ref_batch);
        }
        if (w) page_free(leaf);
        else {
            memset(leaf + first_slot(0), 0, (512-first_slot(0))*sizeof(*leaf));
            directory(p)[0] = link;
        }
    }
    sfence_vma();
    p->branch = NULL;
    struct proc *image = b->image;
    assert(image->template_branches, "template branch underflow");
    --image->template_branches;
    struct template_job *job = image->reclaim_job;
    if (job && job->closed) {
        if (job->policy != TEMPLATE_WINDOW_BATCH) {
            for (unsigned w = 0; w < HEAP_WINDOWS; ++w)
                dirty_words(job, w, job->blocked[w]);
        }
        queue_work(job);
    }
    template_put(image);
    page_free(b);
}
