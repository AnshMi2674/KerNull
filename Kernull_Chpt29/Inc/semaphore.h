/******************************************************************************
 * File        : semaphore.h
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores
 *
 * Description:
 *  A counting/binary semaphore for task<->task and ISR->task
 *  synchronization.
 *
 *  Scope of this chapter:
 *      - Counting semaphore (count = 0, 1, 2, ...)
 *      - Binary semaphore built on the same mechanism (count in {0,1})
 *      - Blocking wait, non-blocking signal
 *      - Signal is legal from an ISR; wait is NOT
 *      - Per-semaphore wait list, highest-priority-waiter wakeup
 *      - Direct handoff on signal (see item 3 below)
 *
 *  Explicitly NOT in this chapter:
 *      - Ownership of any kind (a semaphore has none - see item 1)
 *      - Priority inheritance (that concept doesn't even apply here,
 *        again because of item 1)
 *      - Timeouts on semaphore_wait()
 *
 *============================================================================
 *                              THE THEORY
 *============================================================================
 *
 * 1. SEMAPHORE vs MUTEX - THIS IS THE WHOLE POINT OF THE CHAPTER
 * ------------------------------------------------------------------
 * mutex.h (Chapter 28) already covers races, atomicity, and
 * interrupt-masking-as-critical-section in depth. A semaphore reuses
 * every one of those mechanisms unchanged (Mutex_h_t.h's item 6
 * "Mechanism A", the wait-list/List_t machinery, kernel_wake_task()).
 * What's actually NEW here is the question the primitive answers:
 *
 *      Mutex:     "Who owns this resource right now?"
 *      Semaphore: "How many units of <something> are available?"
 *
 * A mutex is always locked/unlocked by a single, identifiable owner,
 * and the kernel enforces that ("only the owner may unlock" -
 * mutex.c rejects otherwise). A Semaphore_t has NO owner field at
 * all. Any task, and any ISR, may call semaphore_signal() on a given
 * semaphore regardless of which task (if any) is currently blocked
 * on it. This is precisely what makes it the right tool for
 * ISR -> task event notification (see item 4) - a mutex could never
 * be unlocked "by the interrupt that happens to fire," because an
 * ISR is not a task and could never have been the owner in the first
 * place.
 *
 * 2. COUNTING SEMANTICS
 * -----------------------
 * sem->count models "how many tokens are currently free":
 *      3 identical DMA buffers available -> count = 3
 *      3 waits in a row succeed and decrement it -> 3, 2, 1, 0
 *      a 4th wait finds count == 0 and blocks
 *      a signal either hands a waiter its token directly (item 3)
 *      or, with nobody waiting, increments count back up
 * A binary semaphore (semaphore_init_binary()) is nothing but a
 * counting semaphore whose count is only ever allowed to be 0 or 1 -
 * there is no separate data structure or code path for it.
 *
 * 3. DIRECT HANDOFF (same reasoning as mutex.h item 9, replayed here
 *    because a semaphore has no owner to make the argument "obviously
 *    about ownership" the way a mutex's does)
 * ---------------------------------------------------------------------
 * Naive, WRONG signal():
 *      sem->count++;
 *      wake the highest-priority waiter, if any;
 * This is wrong because between the increment and the wake, some
 * OTHER task could call semaphore_wait(), see count > 0, and steal
 * the token that was meant for the waiter that had already been
 * queued first. On a single core this window is only as wide as
 * however long semaphore_signal() itself takes if it isn't
 * critical-sectioned correctly - but it is a real, exploitable
 * ordering bug, not a theoretical one, and it is *silent*: nothing
 * crashes, a task just occasionally loses its turn.
 *
 * Correct semaphore_signal():
 *      if someone is already waiting:
 *          remove them from sem->waitList
 *          move them BLOCKED -> READY directly
 *          count is NOT touched
 *      else:
 *          count++
 * The waiter goes straight from BLOCKED to READY as the direct,
 * intended recipient of this exact signal. count never transiently
 * becomes ">0 with a waiter still queued", so there is no window in
 * which anything else could consume the token first.
 *
 * 4. ISR SIGNALING / EVENT SYNCHRONIZATION
 * ---------------------------------------------
 * semaphore_signal() never blocks (it either does an O(1) list
 * removal + wake, or an O(1) increment), so it is always safe to
 * call from interrupt context. This is what lets a semaphore
 * represent a hardware event:
 *
 *      USART2 byte arrives
 *              |
 *              v
 *      USART2_IRQHandler (ISR context)
 *              |
 *              v
 *      store the byte, semaphore_signal(&sem)
 *              |
 *              v
 *      the task that was blocked in semaphore_wait(&sem) becomes
 *      READY (and preempts immediately if it outranks whatever the
 *      ISR interrupted)
 *
 * The semaphore itself has no idea what "a UART byte" is - it only
 * knows "somebody is now allowed to proceed." Interpreting the event
 * (reading the byte back out, etc.) is entirely the waiting task's
 * job, done in task context, after semaphore_wait() returns.
 * semaphore_wait() must NEVER be called from an ISR for the same
 * reason mutex_lock() can't be (mutex.h item 12): blocking requires
 * a TCB and a "resume me later" story that an ISR does not have.
 *
 * 5. PRIORITY-BASED WAITER SELECTION
 * ---------------------------------------
 * Exactly the same policy as mutex.h item 8, and reusing the exact
 * same scan shape: when semaphore_signal() finds more than one
 * waiter, it wakes the one with the smallest 'priority' value
 * (highest priority), not the one that arrived first. With
 * MAX_TASKS capped at 8, a linear scan of the wait list is a handful
 * of comparisons - not worth a priority-ordered data structure for
 * this educational kernel. Same-priority waiters are served in the
 * order they blocked, because the scan only replaces 'best' on a
 * STRICTLY smaller priority (see semaphore.c) - ties keep whichever
 * candidate was found first, which is the earliest arrival since the
 * wait list is itself FIFO-ordered by list_append().
 *
 * 6. NO NEW TASK STATE
 * -----------------------
 * A semaphore waiter is TASK_BLOCKED - the exact same enum value
 * used for a mutex waiter and (in a future chapter) a sleeping task.
 * What it's blocked FOR is entirely determined by which wait list
 * its TCB currently sits on (here, Semaphore_t.waitList), never by
 * the TaskState itself. See kernel.h's TaskState comment - Chapter
 * 29 needed zero changes there.
 ******************************************************************************/

#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <stdint.h>
#include "kernel.h"
#include "list.h"

/*===========================================================================
 *                          Semaphore Data Structure
 *===========================================================================*/

/*
 * A semaphore has exactly two pieces of state:
 *
 *      count    - how many tokens are currently free to be taken by
 *                 a semaphore_wait() without blocking.
 *      waitList - every task currently blocked in semaphore_wait(),
 *                 using the same generic List_t / TCB_t->next
 *                 machinery every other queue in this kernel uses.
 *
 * Deliberately NO owner field - see mutex.h vs this file, item 1.
 *
 * A Semaphore_t is a plain static/global struct the application
 * declares wherever it needs one (see main.c) - no semaphore pool or
 * allocator, matching this kernel's "no dynamic memory, anywhere"
 * rule.
 */
typedef struct
{
    uint32_t count;
    List_t   waitList;

} Semaphore_t;

/*===========================================================================
 *                              Semaphore API
 *===========================================================================*/

/*
 * Initializes a counting semaphore with 'initialCount' tokens
 * already available, and an empty wait list. Must be called once
 * before a semaphore is used for anything else.
 */
void semaphore_init(Semaphore_t *sem, uint32_t initialCount);

/*
 * Initializes a semaphore for binary use: 'initialState' is clamped
 * so count can only ever be 0 or 1 at init time. This does NOT
 * change the underlying mechanism at all - semaphore_wait() and
 * semaphore_signal() have no idea whether a given Semaphore_t is
 * "meant" to be binary or counting. Keeping count at 0/1 is a
 * discipline the application maintains by only ever calling
 * semaphore_signal() at most once per "event" - the same way mutex.h
 * item 4 (non-recursive mutex) is a documented usage discipline, not
 * something the code enforces at every call site.
 *
 * Remember: binary semaphore != mutex. There is still no owner, and
 * it is perfectly valid (indeed the common case) for Task A to wait
 * and an ISR, or a completely different task, to be the one that
 * signals.
 */
void semaphore_init_binary(Semaphore_t *sem, uint8_t initialState);

/*
 * Waits for a token, blocking the calling task if none is
 * immediately available.
 *
 * Task-context only, for the same reason as mutex_lock() (see
 * mutex.h item 12 / this file's item 4): blocking needs a TCB and a
 * "resume later" story an ISR doesn't have.
 *
 * Returns:
 *      1   A token was obtained. Either it was immediately available,
 *          or this call blocked and was later handed one directly by
 *          semaphore_signal() (see item 3 above) - the caller cannot
 *          tell which happened from the return value, and does not
 *          need to: either way, it now legitimately holds one token.
 *      0   Rejected: 'sem' was NULL, or this was called from an ISR.
 *          No state was changed.
 */
uint8_t semaphore_wait(Semaphore_t *sem);

/*
 * Signals the semaphore: either directly wakes the highest-priority
 * waiter (see item 3, "direct handoff") or, if nobody is waiting,
 * increments count.
 *
 * Legal from BOTH task context and ISR context - see item 4. Never
 * blocks.
 *
 * Returns:
 *      1   Signaled successfully (a waiter was woken, or count was
 *          incremented).
 *      0   Rejected: 'sem' was NULL. No state was changed.
 */
uint8_t semaphore_signal(Semaphore_t *sem);

#endif /* SEMAPHORE_H */
