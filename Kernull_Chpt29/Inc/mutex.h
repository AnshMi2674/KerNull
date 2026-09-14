/******************************************************************************
 * File        : mutex.h
 * Project     : Educational RTOS
 * Chapter     : 28 - Blocking Mutexes
 *
 * Description:
 *  A blocking mutex (mutual exclusion lock) for tasks to protect
 *  shared resources from concurrent access.
 *
 *  Scope of this chapter:
 *      - Atomic lock acquisition
 *      - Blocking instead of spinning
 *      - A per-mutex wait list
 *      - Highest-priority-waiter selection
 *      - Direct ownership handoff on unlock
 *      - Task-context-only restriction (no ISR use)
 *
 *  Explicitly NOT in this chapter (documented as future work below):
 *      - Priority inheritance
 *      - Priority ceiling protocols
 *      - Recursive locking
 *      - Deadlock detection
 *      - Semaphores (Chapter 29)
 *
 *============================================================================
 *                              THE THEORY
 *============================================================================
 *
 * 1. RACE CONDITIONS
 * -------------------
 * "counter++" looks like one operation in C, but on this CPU it is
 * really three: READ counter, ADD 1, WRITE counter back. If a task
 * is preempted between the READ and the WRITE, and another task
 * also reads/modifies/writes the same variable before the first task
 * resumes, one of the increments is silently lost. This is a race
 * condition -- the outcome depends on the exact timing of a
 * preemption that could happen at almost any instant.
 *
 * 2. CRITICAL SECTIONS
 * ---------------------
 * A critical section is any stretch of code that touches shared
 * state and must not have two tasks executing it "at the same time"
 * (which, on a single core, really means: interleaved via
 * preemption). A mutex is how tasks agree to take turns through a
 * critical section -- one at a time, in whatever order they happen
 * to arrive.
 *
 * 3. MUTEX = MUTUAL EXCLUSION
 * ----------------------------
 * The mutex does not "lock the resource" -- the resource is just
 * memory; nothing stops a task from reading it directly regardless
 * of the mutex. What the mutex actually does is coordinate BEHAVIOUR:
 * every task that wants exclusive access agrees, by convention, to
 * call mutex_lock() before touching the resource and mutex_unlock()
 * after. The mutex only works if every piece of code touching that
 * resource honors that convention.
 *
 * 4. WHY THE OBVIOUS CODE IS NOT SAFE
 * -------------------------------------
 *      if (!mutex->locked) {
 *          mutex->locked = true;
 *      }
 * looks correct, but the check and the write are two separate
 * instructions. A context switch (or an ISR) between them can let
 * two tasks both see "!locked", and both then set it, and both
 * believe they hold the mutex. The read-modify-decide sequence needs
 * to happen as ONE indivisible step -- an ATOMIC operation.
 *
 * 5. TEST-AND-SET
 * -----------------
 * The classic atomic primitive this pattern needs is conceptually:
 *      old    = locked;
 *      locked = true;
 *      return old;
 * where the read of the old value and the write of the new value are
 * guaranteed, by the mechanism performing them, to happen with
 * nothing else able to interleave in between. If 'old' comes back
 * FREE, the caller just acquired the lock. If 'old' comes back
 * LOCKED, someone else already had it, and nothing was changed by
 * this attempt.
 *
 * 6. TWO WAYS TO GET ATOMICITY ON CORTEX-M3
 * --------------------------------------------
 *
 *   Mechanism A -- mask interrupts for a few instructions:
 *          CPSID I     // disable all maskable interrupts
 *          ... check + claim, a handful of instructions ...
 *          CPSIE I     // re-enable
 *      On a SINGLE CORE, the only things that could possibly
 *      interleave with this task's code are interrupts (PendSV,
 *      SysTick, USART2, EXTI...) -- there is no second CPU core to
 *      worry about. Masking interrupts for a few instructions makes
 *      those interleavings impossible, which is exactly as strong a
 *      guarantee as "atomic" needs to be, here. THIS IS THE
 *      MECHANISM THIS CHAPTER ACTUALLY USES (kernel_enter_critical()
 *      / kernel_exit_critical() in kernel.c) -- because it is simpler
 *      to reason about and teach than exclusive-access instructions,
 *      and is fully sufficient for a single-core kernel.
 *
 *      A critical rule: NEVER hold this mask while WAITING. Masking
 *      interrupts and then spinning in a while() loop would also
 *      block the timer tick and the very interrupts that might let
 *      the mutex owner run and release it -- a self-inflicted
 *      deadlock. This project's critical sections are only ever a
 *      handful of instructions: check, maybe claim, unmask.
 *
 *      A note on pending interrupts: masking interrupts does not
 *      make them vanish. If, say, USART2 fires while masked, it
 *      becomes PENDING and runs as soon as interrupts are unmasked
 *      again. But the pending mechanism is typically a single bit
 *      per interrupt source, not an unlimited queue -- if the SAME
 *      interrupt source fires a second time while still masked and
 *      still pending from the first occurrence, that second
 *      occurrence can be coalesced away rather than separately
 *      remembered. This is exactly why these critical sections must
 *      stay extremely short: the longer interrupts stay masked, the
 *      more realistic it becomes to actually lose an event this way.
 *
 *   Mechanism B -- LDREX/STREX (exclusive access primitives):
 *          LDREX Rt, [addr]     // load, and mark this address as
 *                               // "exclusively watched"
 *          ... compute new value ...
 *          STREX Rd, Rt2, [addr]// store ONLY IF nothing else wrote
 *                               // to addr since the LDREX; Rd tells
 *                               // you whether it succeeded
 *      This is a genuinely lock-free primitive: it never disables
 *      interrupts, and it works even across multiple CPU cores,
 *      because the exclusive-access monitor is hardware, not a
 *      software mask. It is the RIGHT tool when you cannot -- or do
 *      not want to -- mask interrupts around the operation (e.g. in
 *      certain ISR-safe designs, or on a multi-core part). This
 *      kernel is single-core, and every piece of code that touches
 *      mutex->locked is already code we fully control and can wrap
 *      in a critical section -- so LDREX/STREX would add real
 *      complexity (exclusive monitor semantics, retry loops, a
 *      compiler intrinsic or hand-written asm) for no additional
 *      correctness here. It is documented here, and left available,
 *      for a future SMP port or a future ISR-safe primitive.
 *
 * 7. SPINLOCK VS MUTEX
 * ----------------------
 * A spinlock keeps re-checking the lock in a tight loop:
 *      while (test_and_set(&lock)) { }
 * burning CPU cycles the whole time it waits. On a single-core RTOS
 * that is actively harmful: while task B spins waiting for a mutex
 * that task A holds, B is occupying the CPU that A needs in order to
 * finish its critical section and release the mutex. If B has equal
 * or higher priority than A, B can spin forever and A never runs
 * again -- a self-inflicted livelock. A blocking mutex instead
 * removes the waiting task from the CPU entirely (TASK_BLOCKED, off
 * the ready list) so some OTHER task -- possibly the owner itself --
 * can actually make progress. This is why mutex_lock() below blocks
 * rather than spins.
 *
 * 8. WAIT LISTS AND HIGHEST-PRIORITY-WAITER SELECTION
 * ------------------------------------------------------
 * Every mutex keeps its own list of blocked waiters (Mutex_t.waitList,
 * reusing the existing List_t/TCB_t->next machinery -- see list.h).
 * When multiple tasks are waiting, this chapter's policy is: the
 * HIGHEST-priority waiter (smallest priority number) is the one who
 * gets the mutex when it becomes available, regardless of arrival
 * order. This is a deliberate choice, not FIFO -- a P1 task should
 * not sit behind a P5 task just because the P5 task asked first.
 *
 * 9. OWNERSHIP AND DIRECT HANDOFF
 * -----------------------------------
 * A mutex always knows exactly which task currently owns it. Only
 * that owner may unlock it -- this project rejects an unlock attempt
 * from any other task rather than silently letting it corrupt the
 * mutex's state.
 *
 * When the owner unlocks and waiters exist, this kernel does NOT do
 * the naive thing:
 *      locked = false;   // and let everyone race to grab it
 * Instead it performs DIRECT HANDOFF: the selected waiter becomes the
 * new owner immediately, inside the same critical section that
 * removed it from the wait list, with 'locked' staying true the
 * entire time. No other task ever observes the mutex as "free" in
 * between -- there is no window in which a third task could steal
 * it out from under the waiter that was just chosen.
 *
 * 10. PRIORITY INVERSION (documented, not solved here)
 * --------------------------------------------------------
 * Classic scenario:
 *      A (P5, lowest) owns a mutex.
 *      B (P3) is READY and keeps running -- it doesn't touch the
 *        mutex at all, it's just a normal, unrelated, higher-priority
 *        task than A.
 *      C (P1, highest) wants the mutex and blocks.
 * C is effectively waiting on A, but A can't run (and thus can't
 * finish its critical section and unlock) because B -- who has
 * nothing to do with the mutex -- keeps preempting it. C, the
 * highest-priority task in the system, is indirectly delayed by B, a
 * task with LOWER priority than C. That is priority inversion: the
 * system is behaving as if C's priority had been "inverted" down
 * near A's.
 *
 * The standard fix is PRIORITY INHERITANCE: temporarily boost A's
 * priority to match the highest-priority task waiting on A's mutex
 * (here, up to C's P1) for as long as A holds it, so nothing at B's
 * priority can preempt A out from under C. This chapter does NOT
 * implement priority inheritance -- it is called out here explicitly
 * as a planned RTOS v2/v3 feature. Application code using this
 * chapter's mutex should be aware that priority inversion is a real,
 * currently-unmitigated risk: keep critical sections short, and avoid
 * having a high-priority task depend on a mutex a low-priority task
 * might hold for a long time.
 *
 * 11. DEADLOCK (documented, not solved here)
 * -----------------------------------------------
 * Classic scenario:
 *      Task A: locks M1, then tries to lock M2.
 *      Task B: locks M2, then tries to lock M1.
 * If A gets M1 and B gets M2 before either tries for the second
 * mutex, A blocks waiting for M2 (held by B) and B blocks waiting for
 * M1 (held by A). Neither can ever make progress. This kernel
 * implements no deadlock detection or recovery -- avoiding deadlock
 * is the APPLICATION's responsibility. The standard discipline: pick
 * one global order for every pair of mutexes any task might need
 * together (e.g. "always acquire M1 before M2, never the reverse")
 * and follow it everywhere. This project's own demo (main.c) never
 * holds more than one mutex at a time specifically to avoid needing
 * to reason about lock ordering at all.
 *
 * 12. WHY THIS API IS TASK-CONTEXT ONLY
 * -----------------------------------------
 * mutex_lock() can BLOCK -- it may remove the calling task from
 * execution and switch to a different one. That only makes sense for
 * a TASK: a task has its own TCB, its own saved context, and a
 * well-defined "come back and run me again later" story. An
 * interrupt handler is not a task -- it has no TCB, and "block this
 * ISR and run something else, then come back" is not a thing this
 * architecture (or most RTOS architectures) supports. Calling
 * mutex_lock()/mutex_unlock() from an ISR is therefore rejected (see
 * kernel_in_isr() in kernel.h) rather than allowed to do something
 * undefined.
 ******************************************************************************/

#ifndef MUTEX_H
#define MUTEX_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"
#include "list.h"

/*===========================================================================
 *                          Mutex Data Structure
 *===========================================================================*/

/*
 * A mutex has exactly three pieces of state:
 *
 *      locked   - is anyone holding it right now?
 *      owner    - if locked, exactly which task holds it (needed to
 *                 reject an unlock from any other task).
 *      waitList - every task currently blocked waiting for it, using
 *                 the same generic List_t / TCB_t->next machinery
 *                 every other queue in this kernel uses. No dynamic
 *                 allocation: a waiting task's own TCB (already
 *                 living in taskList[]) is what gets linked in here.
 *
 * A Mutex_t is a plain static/global struct the application declares
 * wherever it needs one (see main.c) -- there is no mutex pool or
 * allocator, matching this kernel's "no dynamic memory, anywhere"
 * rule.
 *
 * KNOWN LIMITATION: this mutex is NOT recursive. If a task that
 * already owns a mutex calls mutex_lock() on it again, it will block
 * waiting for itself to release it -- which can never happen. This
 * is a real, documented misuse case, not a bug this chapter tries to
 * prevent; recursive mutexes are extra complexity this project does
 * not currently need.
 */
typedef struct
{
    volatile bool locked;
    TCB_t        *owner;
    List_t        waitList;

} Mutex_t;

/*===========================================================================
 *                              Mutex API
 *===========================================================================*/

/*
 * Initializes a mutex to the unlocked, unowned, empty-wait-list
 * state. Must be called once before a mutex is used for anything
 * else.
 */
void mutex_init(Mutex_t *mutex);

/*
 * Acquires 'mutex', blocking the calling task if it is not
 * immediately available.
 *
 * Task-context only -- see item 12 above. Calling this from an ISR
 * is rejected without side effects.
 *
 * Returns:
 *      1   Mutex acquired. The calling task now owns it.
 *      0   Rejected: 'mutex' was NULL, or this was called from an
 *          ISR. (This function otherwise always eventually returns 1
 *          -- from task context, acquiring a valid mutex is not
 *          allowed to simply "fail" the way, say, a non-blocking
 *          try-lock would; it blocks for as long as it takes.)
 */
uint8_t mutex_lock(Mutex_t *mutex);

/*
 * Releases 'mutex'. Only the current owner may do this.
 *
 * If another task is waiting, ownership is handed directly to the
 * highest-priority waiter (see item 9 above) -- that task is moved
 * straight from BLOCKED to owning the mutex, without needing to call
 * mutex_lock() again.
 *
 * Task-context only -- see item 12 above.
 *
 * Returns:
 *      1   Unlocked successfully (or handed off successfully).
 *      0   Rejected: 'mutex' was NULL, this was called from an ISR,
 *          or the calling task does not currently own this mutex.
 *          Mutex state is left completely unchanged in this case.
 */
uint8_t mutex_unlock(Mutex_t *mutex);

#endif /* MUTEX_H */
