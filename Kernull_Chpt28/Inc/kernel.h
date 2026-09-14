/******************************************************************************
 * File        : kernel.h
 * Project     : Educational RTOS
 * Chapter     : 28 - Blocking Mutexes
 *
 * Description:
 *  Public interface for the RTOS kernel developed up to Chapter 28.
 *
 *  Everything from Chapter 27 is preserved unchanged:
 *      - Task creation with fixed priority
 *      - Task Control Blocks (TCBs)
 *      - Fake stack initialization
 *      - Real context switching (SVC for first launch, PendSV for every
 *        switch after that)
 *      - Linked-list based, priority-partitioned ready queues
 *
 *  Chapter 28 does NOT touch the scheduler's policy or the CPU
 *  context-switch mechanism at all. It only adds a small set of
 *  GENERIC primitives -- "who is running", "make this happen soon",
 *  "put this task back on the ready list", "am I in an ISR", "give
 *  me a short atomic window" -- that a synchronization primitive
 *  (mutex.c in this chapter, semaphore.c in a later one) can build on
 *  without ever reaching into currentTask, readyList[], or PendSV
 *  directly. This is what keeps the promise that "the scheduler must
 *  not know about mutex internals" literally true: none of the
 *  functions added below know that a Mutex_t exists.
 *
 * Notes:
 *  - Context switching is performed using PendSV.
 *  - The first task is started using an Artificial Exception Return,
 *    reached via SVC (kernel_start() cannot BX LR into Handler mode
 *    directly -- see kernel.c for why).
 *  - Delays/Idle Task and semaphores are not part of this chapter and
 *    will be added later.
 ******************************************************************************/

#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

/*===========================================================================
 *                              Configuration
 *===========================================================================*/

/*
 * Maximum number of tasks supported by the kernel.
 *
 * Every task occupies one Task Control Block (TCB).
 */
#define MAX_TASKS      8

/*
 * Size of each task's private stack.
 *
 * Stack size is measured in 32-bit words.
 *
 * Example:
 * STACK_SIZE = 128
 *
 * Memory used per task:
 *      128 x 4 = 512 bytes
 */
#define STACK_SIZE     128

/*
 * Valid application priority range.
 *
 * Priority 1 is the HIGHEST priority a task can have; priority 5 is
 * the LOWEST. Smaller number = more important.
 */
#define TASK_PRIORITY_MIN   1u
#define TASK_PRIORITY_MAX   5u

/*===========================================================================
 *                              Task States
 *===========================================================================*/

/*
 * Every task exists in exactly one state.
 *
 *      TASK_READY   -> sitting in kernel.c's readyList[priority - 1].
 *      TASK_RUNNING -> currently executing, and in NO list.
 *      TASK_BLOCKED -> waiting for something and, again, in NO ready
 *                      list -- it is instead on whatever wait list
 *                      belongs to the thing it's waiting for.
 *
 * Chapter 28 deliberately does NOT add TASK_MUTEX_WAITING or any
 * other sub-state. A task is BLOCKED for exactly one reason: it
 * cannot make progress right now. WHY it's blocked (waiting for a
 * mutex today; a semaphore, a delay, or I/O in later chapters) is
 * entirely determined by which wait list its TCB currently sits on
 * -- mutex.c's Mutex_t.waitList, in this chapter -- and the
 * scheduler has no need to know or care which one that is. It only
 * ever asks "is this TASK_READY or not".
 */
typedef enum
{
    /*
     * Task is eligible to run.
     * The scheduler may choose it at any context switch.
     */
    TASK_READY,

    /*
     * Task currently executing on the CPU.
     *
     * Since Cortex-M has only one CPU core,
     * only one task can be RUNNING at any instant.
     */
    TASK_RUNNING,

    /*
     * Task cannot execute until some event occurs.
     *
     * As of Chapter 28, this happens when a task calls mutex_lock()
     * on a mutex someone else already owns. Later chapters will add
     * more reasons (a semaphore, a delay) without needing a new
     * enum value -- see the note above.
     */
    TASK_BLOCKED

} TaskState;


/*
 * Non-zero once kernel_start() has launched the first task.
 *
 * SysTick_Handler uses this to decide whether it is safe to request
 * a reschedule. Before this is set, currentTask is NULL, the ready
 * lists may be empty, and pending PendSV would trap inside
 * scheduler_pick_next().
 */
extern volatile uint8_t kernel_running;


/*===========================================================================
 *                          Task Control Block (TCB)
 *===========================================================================*/

/*
 * Every task in the system owns exactly one TCB.
 *
 * The TCB stores everything required to stop a task
 * and resume it later.
 *
 * During a context switch:
 *
 *      Running Task
 *          |
 *          v
 *     Save CPU Context
 *          |
 *          v
 *      Save PSP into TCB
 *
 * -----------------------------
 *
 *      Next Task
 *          |
 *          v
 *     Load PSP from TCB
 *          |
 *          v
 *     Restore CPU Context
 */
typedef struct TCB_t
{
    /*
     * Saved Process Stack Pointer.
     *
     * IMPORTANT: this must remain the FIRST member of TCB_t.
     * The PendSV/SVC assembly takes advantage of the fact that a
     * pointer to a TCB_t and a pointer to its 'sp' field are the
     * same address. Chapter 28 does not touch this layout guarantee
     * -- it adds no new fields at all, in fact; it only adds new
     * functions that operate on the fields already here.
     */
    uint32_t *sp;

    /*
     * Private stack belonging exclusively to this task.
     *
     * Aligned to 8 bytes because the Cortex-M exception entry/exit
     * sequence expects an 8-byte aligned stack pointer at the point
     * an exception frame is pushed/popped.
     */
    uint32_t stack[STACK_SIZE] __attribute__((aligned(8)));

    /*
     * Entry function executed when the task starts.
     */
    void (*taskFunc)(void);

    /*
     * Human-readable task name.
     *
     * Used only for debugging and diagnostics.
     */
    const char *name;

    /*
     * Current execution state.
     */
    TaskState state;

    /*
     * Intrusive linked-list pointer (introduced in Chapter 26).
     *
     * A given TCB is on at most one list at a time: exactly one of
     * "a priority's readyList[]" or "a synchronization primitive's
     * wait list" (e.g. Mutex_t.waitList) -- never both, and never
     * neither while the task is not RUNNING. mutex.c relies on this
     * same field to build its wait list; it needed no changes here.
     */
    struct TCB_t *next;

    /*
     * Fixed scheduling priority (introduced in Chapter 27).
     *
     * Valid range: TASK_PRIORITY_MIN (1, highest) to
     * TASK_PRIORITY_MAX (5, lowest). Never changes after creation.
     * This is also how mutex.c decides which waiter to hand a mutex
     * to: it compares this field across everyone on the wait list
     * and picks the smallest value.
     */
    uint8_t priority;

} TCB_t;

/*===========================================================================
 *                          Kernel API
 *===========================================================================*/

/*
 * Initializes all kernel data structures.
 *
 * Must be called before creating any tasks.
 */
void kernel_init(void);

/*
 * Creates a new task at a fixed priority.
 *
 * The task is immediately appended to the READY queue for that
 * priority -- it does not wait until kernel_start() to become
 * schedulable.
 *
 * Parameters:
 *      taskFunc    Entry function.
 *      name        Human-readable task name.
 *      priority    Fixed priority, 1 (highest) .. 5 (lowest).
 *
 * Returns:
 *      1   Task created successfully.
 *      0   Task list full, OR priority out of range.
 */
uint8_t kernel_create_task(void (*taskFunc)(void),
                           const char *name,
                           uint8_t priority);

/*
 * Starts the RTOS.
 *
 * This function never returns. Once it starts the first task, all
 * future task switches occur through PendSV.
 */
void kernel_start(void);

/*
 * Called once per SysTick interrupt (from SysTick_Handler in main.c).
 *
 * Requests a context switch by pending PendSV; does not perform the
 * switch itself. See kernel_request_reschedule() below -- this
 * function is now just that primitive, named for its specific
 * caller.
 */
void kernel_tick(void);

/*===========================================================================
 *              Chapter 28: Primitives for Synchronization Code
 *===========================================================================*
 *
 * Everything below this line exists so that mutex.c (and later,
 * semaphore.c) can correctly interact with the scheduler WITHOUT the
 * scheduler needing to know that mutexes or semaphores exist. Every
 * function here is expressed purely in terms of TCB_t and generic
 * scheduling state -- none of them take a Mutex_t, know what a wait
 * list is FOR, or care why a task is blocked.
 *===========================================================================*/

/*
 * Returns the currently RUNNING task's TCB pointer.
 *
 * Synchronization code needs to know "who is asking to lock/unlock
 * this mutex" without being able to see the kernel's private
 * currentTask variable directly.
 */
TCB_t *kernel_get_current_task(void);

/*
 * Requests an immediate context switch by pending PendSV, and
 * returns immediately -- the actual switch happens whenever PendSV
 * next gets to run (which, because of its priority, is essentially
 * "as soon as possible" but not literally on this instruction).
 *
 * This function does NOT touch task state and does NOT touch any
 * list. Callers (mutex_lock(), in this chapter) are responsible for
 * having ALREADY updated currentTask->state and placed it on
 * whatever wait list is correct BEFORE calling this -- ideally from
 * inside the same short critical section that made that decision.
 * See mutex_lock() in mutex.c for the worked example.
 *
 * Why this is safe with no further bookkeeping here:
 * scheduler_pick_next() only ever re-appends the outgoing task to a
 * ready list when it finds that task's state is still TASK_RUNNING.
 * A caller that changes the state to TASK_BLOCKED before calling
 * this function is exactly what makes scheduler_pick_next() correctly
 * skip re-queuing it -- with zero changes needed inside the
 * scheduler itself.
 */
void kernel_request_reschedule(void);

/*
 * Moves a BLOCKED task back to READY, inserts it into the ready list
 * for its own fixed priority, and -- if it now outranks whichever
 * task is currently RUNNING -- requests an immediate preemption.
 *
 * This is the counterpart to "a task blocked itself": something
 * external (mutex_unlock() handing off ownership, in this chapter)
 * decided a specific TCB should run again, and this function is how
 * that decision gets folded back into the scheduler's own
 * bookkeeping correctly -- same ready-list-append-plus-preemption-
 * check logic that kernel_create_task() already uses for a brand
 * new task becoming READY for the first time.
 *
 * Caller's responsibility: 'task' must not already be on ANY list
 * (ready or otherwise) when this is called -- e.g. mutex_unlock()
 * must have already removed it from the mutex's wait list first.
 */
void kernel_wake_task(TCB_t *task);

/*
 * Returns non-zero if currently executing inside an exception
 * handler (Handler mode), zero if in normal task (Thread mode)
 * execution.
 *
 * Reads the CPU's IPSR register directly: IPSR is 0 in Thread mode,
 * and holds the current exception number whenever an exception
 * handler (SysTick, PendSV, USART2, EXTI, ...) is active. This is
 * how mutex_lock()/mutex_unlock() enforce that they are task-context
 * only -- see mutex.h for why that restriction exists.
 */
uint8_t kernel_in_isr(void);

/*
 * Enters/exits a short critical section by masking all maskable
 * interrupts (CPSID I / CPSIE I -- i.e. setting/clearing PRIMASK).
 *
 * This is Chapter 28's "Mechanism A" for atomic check-and-claim
 * operations (see the theory discussion at the top of mutex.h). It
 * is intentionally NOT a save/restore of the previous PRIMASK value:
 * every call site in this project uses a plain, non-nested
 * enter/exit pair, so there is nothing to restore -- adding that
 * bookkeeping would only obscure the one property that actually
 * matters here, which is "nothing else can run between enter and
 * exit."
 *
 * These sections must be kept EXTREMELY short (a handful of
 * instructions). See mutex.c for exactly what is, and is not,
 * allowed to happen while interrupts are masked.
 */
void kernel_enter_critical(void);
void kernel_exit_critical(void);

#endif /* KERNEL_H */
