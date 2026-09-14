/******************************************************************************
 * File        : mutex.c
 * Project     : Educational RTOS
 * Chapter     : 28 - Blocking Mutexes (atomicity correction pass, Ch. 29)
 *
 * Description:
 *  Implements the blocking mutex declared in mutex.h. Semantics,
 *  ownership rules, and the public API are UNCHANGED from Chapter 28.
 *
 *  Corrections in this pass:
 *
 *  1. mutex_unlock()'s direct handoff previously removed the waiter
 *     from mutex->waitList inside one critical section, then closed
 *     it, THEN called kernel_wake_task() (which opened a SEPARATE
 *     critical section) to mark it READY and insert it into
 *     readyList[]. Between those two critical sections, 'nextOwner'
 *     was observably on no list at all. Fixed by calling the new
 *     kernel_wake_task_locked() from inside the SAME critical section
 *     that removes the waiter -- one continuous masked window, no
 *     gap. See kernel.h for why this needed a new function rather
 *     than just nesting kernel_enter_critical() calls.
 *
 *  2. mutex_lock()/mutex_unlock() now check kernel_get_current_task()
 *     for NULL before dereferencing it. Calling either from Thread
 *     mode before kernel_start() has ever run would previously
 *     dereference a NULL 'self' -- an unlikely but real misuse case,
 *     guarded the same way semaphore_wait() now is.
 *
 *  3. mutex_lock() now recognizes the direct-handoff resumption case.
 *     When mutex_unlock() transfers ownership to a blocked waiter,
 *     the mutex remains locked and mutex->owner becomes that waiter.
 *     When the waiter resumes inside its original mutex_lock() call,
 *     it must return success instead of looping and blocking again.
 *     A local 'waited' flag ensures this shortcut is only used after
 *     this same call actually blocked, so a non-recursive mutex is
 *     not accidentally turned into a recursive one.
 *
 *  Everything else -- atomicity theory, spinlock-vs-mutex,
 *  priority-inversion/deadlock documentation, ownership rules -- is
 *  unchanged; see mutex.h.
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
 * the numerically smallest 'priority'. Must be called from inside an
 * active kernel_enter_critical() section -- unchanged from Chapter 28.
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

    if (self == 0)
    {
        /*
         * No task has ever started running yet (kernel_start() has
         * not executed SVC). There is nothing to mark BLOCKED and
         * nothing to hand this mutex to -- reject rather than
         * dereference NULL below.
         */
        return 0;
    }

    uint8_t waited = 0;

    for (;;)
    {
        kernel_enter_critical();

        if (!mutex->locked)
        {
            mutex->locked = true;
            mutex->owner  = self;

            kernel_exit_critical();

            return 1;
        }

        /*
         * Direct-handoff resumption:
         *
         * If THIS mutex_lock() call already blocked once, and the
         * owner then called mutex_unlock(), mutex_unlock() selected
         * this task as the next owner and woke it. The mutex is still
         * locked (locked stays true during handoff), and owner is
         * already self. Do not block again: the original call has
         * now acquired the mutex.
         *
         * 'waited' is required so that a task which already owns the
         * mutex and calls mutex_lock() again does NOT get a false
         * success. This kernel's mutexes are documented as
         * non-recursive; the recursive misuse case should still
         * deadlock rather than silently succeed.
         */
        if (waited && mutex->owner == self)
        {
            kernel_exit_critical();

            return 1;
        }

        self->state = TASK_BLOCKED;
        list_append(&mutex->waitList, self);
        waited = 1;

        kernel_exit_critical();

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

    if (self == 0)
    {
        return 0;
    }

    kernel_enter_critical();

    if (mutex->owner != self)
    {
        kernel_exit_critical();
        return 0;
    }

    TCB_t *nextOwner = mutex_pick_highest_priority_waiter(mutex);

    if (nextOwner == 0)
    {
        mutex->locked = false;
        mutex->owner  = 0;

        kernel_exit_critical();

        return 1;
    }

    /*
     * Direct handoff. 'nextOwner' was already removed from
     * mutex->waitList by the picker above, so it is on NO list right
     * now -- exactly the precondition kernel_wake_task_locked()
     * requires. Calling it HERE, before kernel_exit_critical(), is
     * this pass's fix: the removal above and this wake-up are now
     * one continuous masked-interrupt transition, with no instant in
     * between where 'nextOwner' is observably on neither list.
     */
    mutex->owner = nextOwner;

    kernel_wake_task_locked(nextOwner);

    kernel_exit_critical();

    return 1;
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
