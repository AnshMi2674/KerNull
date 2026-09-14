/******************************************************************************
 * File        : mutex.c
 * Project     : Educational RTOS
 * Chapter     : 28 - Blocking Mutexes
 *
 * Description:
 *  Implements the blocking mutex declared in mutex.h.
 *
 *  See mutex.h for the full theory (race conditions, atomicity,
 *  interrupt masking vs LDREX/STREX, spinlocks vs mutexes, priority
 *  inversion, deadlock). This file is the mechanism; mutex.h is the
 *  reasoning behind it.
 *
 *  This file never touches currentTask, readyList[], or PendSV
 *  directly -- everything it needs from the scheduler comes through
 *  the small generic primitives kernel.c added in this same chapter
 *  (kernel_get_current_task(), kernel_request_reschedule(),
 *  kernel_wake_task(), kernel_in_isr(), kernel_enter_critical() /
 *  kernel_exit_critical()). This is what keeps the scheduler
 *  completely unaware that mutexes exist.
 ******************************************************************************/

#include "mutex.h"

/*===========================================================================
 *                          Mutex Initialization
 *===========================================================================*/

void mutex_init(Mutex_t *mutex)
{
    mutex->locked = false;
    mutex->owner  = 0;

    list_init(&mutex->waitList);
}

/*===========================================================================
 *              Highest-Priority-Waiter Selection (internal)
 *===========================================================================*/

/*
 * Scans the mutex's wait list and removes/returns the waiter with
 * the numerically smallest 'priority' (i.e. the highest-priority
 * task waiting). Returns 0 (NULL) if the wait list is empty.
 *
 * A plain O(n) scan over the wait list, walking it via the public
 * head/next fields List_t and TCB_t already expose -- this does not
 * require any change to list.h/list.c, which stay completely unaware
 * that "priority" or "highest-priority selection" exist. list_remove()
 * (already generic) is what actually splices the chosen node out.
 *
 * With at most MAX_TASKS waiters possible (and MAX_TASKS = 8 in this
 * project), this scan is a handful of comparisons at worst -- not
 * worth a more complex priority-ordered data structure.
 *
 * Must be called from inside a kernel_enter_critical() /
 * kernel_exit_critical() section: it mutates mutex->waitList, which
 * mutex_lock() can also be mutating concurrently if a context switch
 * were allowed to happen mid-scan.
 */
static TCB_t *mutex_pick_highest_priority_waiter(Mutex_t *mutex)
{
    TCB_t *best = mutex->waitList.head;

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

    list_remove(&mutex->waitList, best);

    return best;
}

/*===========================================================================
 *                              mutex_lock
 *===========================================================================*/

uint8_t mutex_lock(Mutex_t *mutex)
{
    if (mutex == 0)
    {
        return 0;
    }

    if (kernel_in_isr())
    {
        return 0;
    }

    TCB_t *self = kernel_get_current_task();

    for (;;)
    {
        kernel_enter_critical();

        /*
         * Direct-handoff case: mutex_unlock() may have transferred
         * ownership straight to us while we were blocked, WITHOUT
         * ever clearing 'locked'. In that case we are already the
         * owner and must simply return.
         *
         * This check MUST come before the '!locked' check below:
         * after a direct handoff, 'locked' is still true, so a
         * naive first-touch check would send us straight back into
         * the blocking branch -- blocking us on a mutex we already
         * own, forever.
         */
        if (mutex->owner == self)
        {
            kernel_exit_critical();
            return 1;
        }

        /* Normal fresh acquisition: nobody owns it, claim it. */
        if (!mutex->locked)
        {
            mutex->locked = true;
            mutex->owner  = self;

            kernel_exit_critical();
            return 1;
        }

        /*
         * Someone else holds it: block. State change and enqueue
         * both happen inside the same critical section that made
         * the decision -- otherwise a SysTick between them could
         * schedule us while we're BLOCKED but not on any list yet.
         */
        self->state = TASK_BLOCKED;
        list_append(&mutex->waitList, self);

        kernel_exit_critical();

        /*
         * Yield. We resume here when mutex_unlock() directly hands
         * ownership to us and calls kernel_wake_task(), at which
         * point the 'owner == self' check at the top of the loop
         * will return 1.
         */
        kernel_request_reschedule();
    }
}
/*===========================================================================
 *                              mutex_unlock
 *===========================================================================*/

uint8_t mutex_unlock(Mutex_t *mutex)
{
    if (mutex == 0)
    {
        return 0;
    }

    if (kernel_in_isr())
    {
        return 0;
    }

    TCB_t *self = kernel_get_current_task();

    kernel_enter_critical();

    /*
     * Ownership check (mutex.h item 9): only the owner may unlock.
     * Reject cleanly and leave every field untouched -- corrupting
     * mutex state on a misuse is worse than just refusing the call.
     */
    if (mutex->owner != self)
    {
        kernel_exit_critical();
        return 0;
    }

    TCB_t *nextOwner = mutex_pick_highest_priority_waiter(mutex);

    if (nextOwner == 0)
    {
        /* Case 1: nobody waiting -- the mutex simply becomes free. */
        mutex->locked = false;
        mutex->owner  = 0;

        kernel_exit_critical();

        return 1;
    }

    /*
     * Case 2: direct handoff (mutex.h item 9). Ownership transfers
     * to 'nextOwner' immediately, without 'locked' ever becoming
     * false -- so no third task calling mutex_lock() in between can
     * observe the mutex as free and race in to steal it. 'nextOwner'
     * was already removed from mutex->waitList by the picker above,
     * so it is not on ANY list right now, which is exactly the state
     * kernel_wake_task() expects.
     */
    mutex->owner = nextOwner;

    kernel_exit_critical();

    /*
     * Moving 'nextOwner' from BLOCKED back to READY -- appending it
     * to its own priority's ready list and requesting a preemption
     * if it now outranks whoever is currently running -- is
     * scheduler bookkeeping, not mutex bookkeeping. Delegating it to
     * kernel_wake_task() is what keeps that logic out of this file.
     * It is called OUTSIDE the critical section above: it does its
     * own, separately-scoped scheduler updates, and there is nothing
     * left in mutex->waitList/mutex->owner for it to race with here.
     */
    kernel_wake_task(nextOwner);

    return 1;
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
