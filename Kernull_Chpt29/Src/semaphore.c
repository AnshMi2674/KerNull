/******************************************************************************
 * File        : semaphore.c
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores (atomicity correction pass)
 *
 * Description:
 *  Implements the counting/binary semaphore declared in semaphore.h.
 *  Semantics and the public API are UNCHANGED: same count/waitList
 *  behaviour, same direct-handoff rule, same highest-priority-waiter-
 *  with-FIFO-ties selection, same ISR-legal signal / task-context-
 *  only wait split.
 *
 *  Two corrections in this pass:
 *
 *  1. THE FIX THIS PASS EXISTS FOR: semaphore_signal()'s direct
 *     handoff previously removed the waiter from sem->waitList inside
 *     one critical section, closed it, THEN called kernel_wake_task()
 *     (a SEPARATE critical section) to mark it READY and insert it
 *     into readyList[]. Between those two critical sections, the
 *     waiter was observably on no list at all -- interrupts were
 *     fully enabled during that gap. Fixed by calling the new
 *     kernel_wake_task_locked() from inside the SAME critical section
 *     that removes the waiter, so the whole transition (pick, remove,
 *     mark READY, insert, preemption check) is one continuous
 *     interrupt-masked window. See kernel.h for exactly why this
 *     needed a new "_locked" primitive rather than simply nesting
 *     kernel_enter_critical() calls (short version: this kernel's
 *     critical sections don't nest -- an inner kernel_exit_critical()
 *     would end the outer one early and reopen the very gap being
 *     closed here).
 *
 *  2. semaphore_wait() now checks kernel_get_current_task() for NULL
 *     before dereferencing it, guarding against being called from
 *     Thread mode before kernel_start() has ever run.
 *
 *  Nothing about semaphore_signal() being legal from ISR context
 *  changes: kernel_wake_task_locked() only ever touches readyList[]
 *  and requests PendSV via a plain register write, exactly like the
 *  version it replaces -- both are already safe to do from Handler
 *  mode, and CPSID/CPSIE (this file's critical section) are ordinary
 *  instructions with no restriction against being executed from
 *  inside an ISR either.
 ******************************************************************************/

#include "semaphore.h"

/*===========================================================================
 *                          Semaphore Initialization
 *===========================================================================*/

void semaphore_init(Semaphore_t *sem, uint32_t initialCount)
{
    sem->count = initialCount;

    list_init(&sem->waitList);
}

void semaphore_init_binary(Semaphore_t *sem, uint8_t initialState)
{
    semaphore_init(sem, (initialState != 0) ? 1u : 0u);
}

/*===========================================================================
 *              Highest-Priority-Waiter Selection (internal)
 *===========================================================================*/

/*
 * Scans the semaphore's wait list and removes/returns the waiter
 * with the numerically smallest 'priority'. Ties keep the first
 * (earliest-queued) candidate found, since list_append() is FIFO and
 * this loop only replaces 'best' on a STRICTLY smaller priority --
 * this is what gives same-priority waiters FIFO fairness.
 *
 * Must be called from inside an active kernel_enter_critical()
 * section -- unchanged from before this pass. What changed is what
 * the CALLER does with the result while still holding that same
 * section (see semaphore_signal() below).
 */
static TCB_t *semaphore_pick_highest_priority_waiter(Semaphore_t *sem)
{
    TCB_t *best = sem->waitList.head;

    if (best == 0)
    {
        return 0;
    }

    for (TCB_t *candidate = best->next; candidate != 0; candidate = candidate->next)
    {
        if (candidate->priority < best->priority)
        {
            best = candidate;
        }
    }

    list_remove(&sem->waitList, best);

    return best;
}

/*===========================================================================
 *                              semaphore_wait
 *===========================================================================*/

uint8_t semaphore_wait(Semaphore_t *sem)
{
    if (sem == 0)
    {
        return 0;
    }

    /* Task-context only -- an ISR has no TCB and no "resume later"
     * story, so blocking makes no sense here. */
    if (kernel_in_isr())
    {
        return 0;
    }

    TCB_t *self = kernel_get_current_task();

    if (self == 0)
    {
        /*
         * No task has ever started running yet (kernel_start() has
         * not executed SVC). There is nothing to mark BLOCKED --
         * reject rather than dereference NULL below.
         */
        return 0;
    }

    kernel_enter_critical();

    if (sem->count > 0)
    {
        /* Case A: a token is already available - take it, don't block. */
        sem->count--;

        kernel_exit_critical();

        return 1;
    }

    /*
     * Case B: no token available. Mark ourselves BLOCKED and enqueue
     * on this semaphore's wait list BEFORE re-enabling interrupts --
     * if a SysTick (or any other) interrupt fired between setting the
     * state and joining the wait list, scheduler_pick_next() could
     * run while this task says BLOCKED but isn't on ANY list yet,
     * leaking it forever.
     */
    self->state = TASK_BLOCKED;
    list_append(&sem->waitList, self);

    kernel_exit_critical();

    /*
     * Yield the CPU. Direct handoff (see file header and
     * semaphore_signal() below) guarantees the ONLY way this task
     * leaves sem->waitList and becomes READY again is a
     * semaphore_signal() that chose it specifically -- there is
     * nothing left to re-check when execution resumes here.
     */
    kernel_request_reschedule();

    return 1;
}

/*===========================================================================
 *                              semaphore_signal
 *===========================================================================*/

uint8_t semaphore_signal(Semaphore_t *sem)
{
    if (sem == 0)
    {
        return 0;
    }

    /*
     * No kernel_in_isr() rejection here, and deliberately no
     * kernel_get_current_task() call at all -- semaphore_signal()
     * doesn't need to know who is running to do its job, unlike
     * mutex_unlock() which must check ownership. This is also exactly
     * what keeps this function correct when called from an ISR: it
     * never needs a "self" TCB that an ISR wouldn't have.
     */
    kernel_enter_critical();

    TCB_t *waiter = semaphore_pick_highest_priority_waiter(sem);

    if (waiter == 0)
    {
        /* Nobody waiting - the token becomes available for a future
         * semaphore_wait() to pick up. */
        sem->count++;

        kernel_exit_critical();

        return 1;
    }

    /*
     * Direct handoff -- THE FIX. 'waiter' was already removed from
     * sem->waitList by the picker above, so it is on NO list right
     * now: exactly the precondition kernel_wake_task_locked()
     * requires. Calling it HERE, before kernel_exit_critical(), means
     * the removal above and this wake-up happen as one continuous
     * masked-interrupt transition -- there is no instant, visible to
     * any other interrupt, where 'waiter' is on neither
     * sem->waitList nor a readyList[]. count is deliberately left
     * untouched: this signal is being consumed directly by 'waiter',
     * not deposited into the count for anyone else to possibly grab
     * first.
     */
    kernel_wake_task_locked(waiter);

    kernel_exit_critical();

    return 1;
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
