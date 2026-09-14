/******************************************************************************
 * File        : kernel.h
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores (atomicity correction pass)
 *
 * Description:
 *  Public interface for the RTOS kernel. Everything from Chapters
 *  26-28 is preserved unchanged: fixed-priority scheduling, Round
 *  Robin within a priority, PendSV/SVC context switching, the
 *  generic list machinery, and the mutex-support primitives added in
 *  Chapter 28.
 *
 *  ONE addition in this pass: kernel_wake_task_locked(). See its doc
 *  comment below for why it exists -- in short, mutex_unlock() and
 *  semaphore_signal() both need to remove a waiter from their own
 *  wait list AND make it READY in the scheduler's own bookkeeping as
 *  ONE uninterruptible transition, and the previous kernel_wake_task()
 *  could not be composed into a caller's own critical section without
 *  either leaving a gap or nesting kernel_enter_critical() calls
 *  (which this kernel's critical sections are explicitly not designed
 *  to support -- see kernel_enter_critical()'s doc comment).
 ******************************************************************************/

#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

/*===========================================================================
 *                              Configuration
 *===========================================================================*/

#define MAX_TASKS      8
#define STACK_SIZE     128

#define TASK_PRIORITY_MIN   1u
#define TASK_PRIORITY_MAX   5u

/*===========================================================================
 *                              Task States
 *===========================================================================*/

/*
 * TASK_READY   -> sitting in kernel.c's readyList[priority - 1].
 * TASK_RUNNING -> currently executing, and in NO list.
 * TASK_BLOCKED -> waiting for something, and in NO ready list --
 *                 instead on whatever wait list belongs to the thing
 *                 it's waiting for (a Mutex_t.waitList, a
 *                 Semaphore_t.waitList, ...). No sub-states are added
 *                 per reason: the scheduler only ever asks "is this
 *                 TASK_READY or not" and never needs to know why a
 *                 task is BLOCKED.
 */
typedef enum
{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED

} TaskState;

/*
 * Non-zero once restore_context() has completed its mode switch
 * (PSP valid, CONTROL set) and is about to perform the exception
 * return into the first task. Before this is set, currentTask is
 * NULL and PendSV would trap inside scheduler_pick_next().
 */
extern volatile uint8_t kernel_running;

/*===========================================================================
 *                          Task Control Block (TCB)
 *===========================================================================*/

typedef struct TCB_t
{
    /*
     * Saved Process Stack Pointer. MUST remain the FIRST member --
     * the PendSV/SVC assembly relies on a pointer to a TCB_t and a
     * pointer to its 'sp' field being the same address.
     */
    uint32_t *sp;

    uint32_t stack[STACK_SIZE] __attribute__((aligned(8)));

    void (*taskFunc)(void);

    const char *name;

    TaskState state;

    /*
     * Intrusive linked-list pointer. A given TCB is on at most one
     * list at a time: exactly one of "a priority's readyList[]" or
     * "some synchronization primitive's wait list" -- never both,
     * never neither while the task is not RUNNING.
     */
    struct TCB_t *next;

    /*
     * Fixed scheduling priority. TASK_PRIORITY_MIN (1, highest) to
     * TASK_PRIORITY_MAX (5, lowest). Never changes after creation.
     */
    uint8_t priority;

} TCB_t;

/*===========================================================================
 *                          Kernel API
 *===========================================================================*/

void kernel_init(void);

uint8_t kernel_create_task(void (*taskFunc)(void),
                           const char *name,
                           uint8_t priority);

void kernel_start(void);

void kernel_tick(void);

/*===========================================================================
 *              Primitives for Synchronization Code
 *===========================================================================*
 *
 * mutex.c and semaphore.c interact with the scheduler only through
 * these functions -- none of them know a Mutex_t or Semaphore_t
 * exists.
 *===========================================================================*/

/*
 * Returns the currently RUNNING task's TCB pointer, or 0 (NULL) if
 * called before kernel_start() has ever run (no task has started
 * executing yet). Synchronization code that blocks (mutex_lock(),
 * semaphore_wait()) must check for NULL before dereferencing this --
 * see mutex.c / semaphore.c for the guard.
 */
TCB_t *kernel_get_current_task(void);

/*
 * Requests an immediate context switch by pending PendSV, and
 * returns immediately. Does NOT touch task state or any list --
 * callers must have already updated currentTask->state and placed it
 * on the correct wait list BEFORE calling this, from inside the same
 * critical section that made that decision.
 */
void kernel_request_reschedule(void);

/*
 * Moves a BLOCKED task back to READY, inserts it into its priority's
 * ready list, and requests a preemption if it now outranks whichever
 * task is currently RUNNING -- as ONE self-contained atomic operation
 * (it masks interrupts itself for the duration).
 *
 * Safe to call any time the caller does NOT already hold an active
 * kernel_enter_critical() section. If the caller DOES already hold
 * one (e.g. it just finished removing this same task from a mutex's
 * or semaphore's own wait list, and wants that removal and this
 * wake-up to be one indivisible transition with no externally
 * observable gap in between), call kernel_wake_task_locked() instead
 * -- see that function's doc comment for exactly why, and for the
 * nesting hazard calling this function from inside an existing
 * critical section would create.
 */
void kernel_wake_task(TCB_t *task);

/*
 * The "raw", non-self-protecting twin of kernel_wake_task(): performs
 * the exact same three steps (mark READY, append to
 * readyList[task->priority - 1], request preemption if warranted)
 * but does NOT call kernel_enter_critical() / kernel_exit_critical()
 * itself.
 *
 * MUST be called only from inside a critical section the CALLER
 * already holds (i.e. sometime after the caller's own
 * kernel_enter_critical() and before its matching
 * kernel_exit_critical()). This is what lets mutex_unlock() and
 * semaphore_signal() fold "remove the waiter from my own wait list"
 * and "hand it back to the scheduler" into ONE continuous
 * interrupt-masked window, with no instant in between where the task
 * is observably on neither list.
 *
 * Why this has to be a separate function rather than just having
 * mutex.c/semaphore.c call kernel_wake_task() (which masks interrupts
 * itself) from inside their own already-masked section: this
 * kernel's critical sections are a plain CPSID/CPSIE pair with no
 * save/restore of the previous PRIMASK state (see
 * kernel_enter_critical()) -- they are explicitly not designed to
 * nest. Calling kernel_enter_critical() again while already inside
 * one is a harmless no-op, but the INNER kernel_exit_critical() would
 * unconditionally clear PRIMASK and re-enable interrupts, ending the
 * OUTER critical section early -- silently reintroducing exactly the
 * gap this function exists to close. kernel_wake_task_locked() sidesteps
 * that entirely by never touching PRIMASK at all; the one enclosing
 * kernel_enter_critical()/kernel_exit_critical() pair the caller
 * already holds is the only one in effect.
 */
void kernel_wake_task_locked(TCB_t *task);

/*
 * Returns non-zero if currently executing inside an exception
 * handler (Handler mode), zero if in normal task (Thread mode)
 * execution. Reads IPSR directly.
 */
uint8_t kernel_in_isr(void);

/*
 * Enters/exits a short critical section by masking all maskable
 * interrupts (CPSID I / CPSIE I -- PRIMASK). Intentionally NOT a
 * save/restore of the previous PRIMASK value: every call site in
 * this project uses exactly one, non-nested enter/exit pair per
 * critical section. This remains true after this pass:
 * kernel_wake_task_locked() does not add a second one -- it relies
 * entirely on whichever single pair the caller (mutex_unlock(),
 * semaphore_signal()) already has open.
 *
 * Keep every critical section EXTREMELY short: a linear scan of a
 * wait list bounded by MAX_TASKS, a couple of pointer updates, and a
 * single register write is the ceiling anything in this kernel ever
 * does inside one.
 */
void kernel_enter_critical(void);
void kernel_exit_critical(void);

#endif /* KERNEL_H */
