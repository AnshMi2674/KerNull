/******************************************************************************
 * File        : kernel.c
 * Project     : Educational RTOS
 * Chapter     : 28 - Blocking Mutexes
 *
 * Description:
 *  Implements the kernel developed up to Chapter 28.
 *
 * Features (all preserved from Chapter 27):
 *  - Fixed-priority preemptive scheduling with Round Robin per level
 *  - Task creation
 *  - Fake stack initialization
 *  - Task startup via SVC (Artificial Exception Return)
 *  - Context switching via PendSV
 *  - Linked-list based task queues (list.h / list.c)
 *
 * New in Chapter 28:
 *  - A handful of small, mutex-agnostic primitives that mutex.c
 *    builds on: kernel_get_current_task(), kernel_request_reschedule(),
 *    kernel_wake_task(), kernel_in_isr(), kernel_enter_critical(),
 *    kernel_exit_critical(). None of these know a Mutex_t exists --
 *    they only understand TCB_t and the scheduler's own bookkeeping.
 *    This is deliberate: it is what lets mutex.c implement blocking
 *    and priority-ordered wakeup without the scheduler ever being
 *    aware that mutexes exist.
 *
 * What did NOT change
 * --------------------
 *  Every line of the SVC/PendSV assembly, scheduler_pick_next(), and
 *  pick_highest_priority_ready_task() is identical to Chapter 27.
 *  Blocking a task for a mutex works entirely by setting
 *  TASK_BLOCKED and NOT re-queuing it -- a case
 *  scheduler_pick_next() already handled correctly since Chapter 26
 *  (it only re-appends the outgoing task when it finds
 *  TASK_RUNNING). Chapter 28 needed to add zero scheduling logic to
 *  make blocking work; it only needed a safe way for outside code to
 *  trigger the existing mechanism.
 ******************************************************************************/

#include <stdint.h>
#include "kernel.h"
#include "list.h"

/*===========================================================================
 *                      Core Register Addresses Used Here
 *===========================================================================*/

/*
 * Interrupt Control and State Register.
 * Bit 28 (PENDSVSET) requests PendSV without needing NVIC involvement --
 * PendSV is a system exception, always enabled, controlled here and via
 * its priority in SHPR3 below.
 */
#define SCB_ICSR        (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET  (1u << 28)

/*
 * System Handler Priority Register 3.
 * Bits [23:16] = PendSV priority.
 * Bits [31:24] = SysTick priority.
 *
 * PendSV is deliberately given the lowest possible priority (0xFF).
 * This is what guarantees a context switch only ever happens after
 * every other pending interrupt (USART2, EXTI, SysTick itself) has
 * finished running -- PendSV never preempts another ISR.
 */
#define SCB_SHPR3           (*(volatile uint32_t *)0xE000ED20)
#define SHPR3_PENDSV_LOWEST (0xFFu << 16)

volatile uint8_t kernel_running = 0;

/*===========================================================================
 *                          Kernel Private Data
 *===========================================================================*/

/*
 * Master list containing every task created in the system.
 *
 * taskList[] owns every TCB for its entire lifetime. Ready queues
 * and wait lists (mutex.c's Mutex_t.waitList, in this chapter) never
 * hold a TCB that isn't also a slot in this array -- they only
 * borrow the 'next' field that already lives inside each TCB_t.
 */
static TCB_t taskList[MAX_TASKS];

/*
 * Number of tasks currently created.
 */
static uint8_t taskCount = 0;

/*
 * Number of distinct priority levels the kernel supports.
 */
#define NUM_PRIORITIES (TASK_PRIORITY_MAX - TASK_PRIORITY_MIN + 1u)

/*
 * One READY queue per priority level. readyList[0] is priority 1
 * (highest) through readyList[NUM_PRIORITIES - 1] at priority 5
 * (lowest). See kernel.h / pick_highest_priority_ready_task() for
 * how this is searched.
 */
static List_t readyList[NUM_PRIORITIES];

/*
 * Tasks waiting on something that isn't priority-based READY
 * scheduling. Not used by anything in this chapter -- mutex waiters
 * live on their own Mutex_t.waitList, not here. Declared and
 * initialized so a future chapter (e.g. sleeping tasks) has the
 * storage ready without needing to touch kernel_init() again.
 */
static List_t blockedList;

/*
 * Pointer to the currently RUNNING task's TCB.
 *
 * This IS the pointer the PendSV/SVC assembly dereferences directly
 * -- no separate mirror variable. The currently running task is
 * never a member of any list while it holds this position.
 *
 * volatile + used: see Chapter 26/27 for why (written by C code,
 * read by inline asm the compiler's dataflow analysis can't see).
 */
static TCB_t *volatile currentTask __attribute__((used));

/*===========================================================================
 *                          Kernel Initialization
 *===========================================================================*/

void kernel_init(void)
{
    taskCount   = 0;
    currentTask = 0;
    kernel_running = 0;
    for (uint8_t i = 0; i < NUM_PRIORITIES; i++)
    {
        list_init(&readyList[i]);
    }

    list_init(&blockedList);
}

/*===========================================================================
 *                          Task Exit Handler
 *===========================================================================*/

/*
 * A task should NEVER return. Any task that does falls in here,
 * because kernel_create_task() pre-loads LR with this address.
 */
static void task_exit_error(void)
{
    while (1)
    {
        /* Future improvement: print task name, blink LED, breakpoint. */
    }
}

/*===========================================================================
 *              Priority Preemption Request (Chapter 27)
 *===========================================================================*/

/*
 * Checks whether a task that just became READY should immediately
 * preempt whoever is currently running, and if so, requests PendSV.
 *
 * Chapter 27 had exactly one caller: kernel_create_task(). Chapter
 * 28 adds a second: kernel_wake_task(), used when mutex_unlock()
 * hands ownership directly to a waiter. Both are "a task just became
 * READY that wasn't a moment ago" events, and both need the exact
 * same "does this outrank currentTask" check -- factoring it out
 * once here, rather than duplicating the comparison at each call
 * site, is what keeps that rule expressed in exactly one place.
 */
static void kernel_request_preemption_if_needed(TCB_t *readyTask)
{
    if (currentTask == 0)
    {
        /* Nothing running yet (kernel_start() hasn't executed SVC). */
        return;
    }

    /* Smaller priority number = more important (see kernel.h). */
    if (readyTask->priority < currentTask->priority)
    {
        SCB_ICSR = ICSR_PENDSVSET;
    }
}

/*===========================================================================
 *                          Task Creation
 *===========================================================================*/

uint8_t kernel_create_task(void (*taskFunc)(void), const char *name, uint8_t priority)
{
    if (taskCount >= MAX_TASKS) return 0;
    if (priority < TASK_PRIORITY_MIN || priority > TASK_PRIORITY_MAX) return 0;

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;
    taskList[taskCount].priority = priority;

    uint32_t *sp = &taskList[taskCount].stack[STACK_SIZE];

    /* Hardware exception frame — xPSR first, R0 last. */
    *(--sp) = 0x01000000;                 /* xPSR */
    *(--sp) = (uint32_t)taskFunc;         /* PC   */
    *(--sp) = (uint32_t)task_exit_error;  /* LR   */
    *(--sp) = 0;                          /* R12  */
    *(--sp) = 0;                          /* R3   */
    *(--sp) = 0;                          /* R2   */
    *(--sp) = 0;                          /* R1   */
    *(--sp) = 0;                          /* R0   */

    /* Software-saved half — exactly once. */
    *(--sp) = 0;      /* R11 */
    *(--sp) = 0;      /* R10 */
    *(--sp) = 0;      /* R9  */
    *(--sp) = 0;      /* R8  */
    *(--sp) = 0;      /* R7  */
    *(--sp) = 0;      /* R6  */
    *(--sp) = 0;      /* R5  */
    *(--sp) = 0;      /* R4  */

    taskList[taskCount].sp = sp;

    list_append(&readyList[priority - 1], &taskList[taskCount]);
    kernel_request_preemption_if_needed(&taskList[taskCount]);

    taskCount++;
    return 1;
}

/*===========================================================================
 *                  Fixed-Priority Round Robin Scheduler
 *===========================================================================*/

/*
 * Scans priority 1 through priority 5, in order, and pops the front
 * task off the first non-empty queue it finds. See Chapter 27 for
 * the full rationale (plain linear scan, deliberately not a
 * bitmap/heap, because five priorities makes that unnecessary).
 *
 * Returns 0 (NULL) if every ready queue is empty -- treated by
 * callers as a fatal kernel design error, since Chapter 28 still has
 * no Idle Task.
 */
static TCB_t *pick_highest_priority_ready_task(void)
{
    for (uint8_t priorityIndex = 0; priorityIndex < NUM_PRIORITIES; priorityIndex++)
    {
        if (readyList[priorityIndex].head != 0)
        {
            return list_pop_front(&readyList[priorityIndex]);
        }
    }

    return 0;
}

/*
 * Selects the next task to run.
 *
 *      1. If the outgoing task is still RUNNING (it simply used up
 *         its timeslice), put it back to READY and append it to the
 *         back of ITS OWN priority's ready list.
 *      2. Otherwise (Chapter 28: its state is now TASK_BLOCKED,
 *         because it called mutex_lock() on an unavailable mutex),
 *         do NOT touch any ready list for it at all -- it is already
 *         correctly parked on Mutex_t.waitList by mutex_lock(), and
 *         re-appending it here would put the same TCB on two lists
 *         at once.
 *      3. Ask pick_highest_priority_ready_task() for the next task.
 *
 * This function required ZERO changes to support mutex blocking.
 * The "if RUNNING" check was already exactly the right condition;
 * mutex_lock() setting the state to TASK_BLOCKED before requesting a
 * reschedule is what makes this correctly skip re-queuing it.
 */
static void scheduler_pick_next(void) __attribute__((used));
static void scheduler_pick_next(void)
{
    if (currentTask->state == TASK_RUNNING)
    {
        currentTask->state = TASK_READY;
        list_append(&readyList[currentTask->priority - 1], currentTask);
    }

    currentTask = pick_highest_priority_ready_task();

    if (currentTask == 0)
    {
        /*
         * Every ready queue was empty. With no Idle Task yet, this
         * can only mean a genuine kernel bug -- trap loudly instead
         * of jumping through a NULL pointer a few lines below.
         */
        for (;;)
        {
        }
    }

    currentTask->state = TASK_RUNNING;
}

/*===========================================================================
 *                  Shared Context Restore (used by both
 *                  SVC_Handler and PendSV_Handler)
 *===========================================================================*/

/*
 * Loads whichever task currentTask points at and performs the
 * exception return into it. Unchanged from Chapter 27 -- see that
 * chapter's comments for the full explanation of every instruction.
 *
 * naked + used: must contain nothing but these exact instructions
 * (no compiler prologue/epilogue), and is reached only via "B
 * restore_context" from other naked functions' inline asm, which the
 * compiler's normal call-graph analysis can't see.
 */

__attribute__((naked, used))
static void restore_context(void)
{
    __asm volatile
    (
        "LDR   R0, =currentTask    \n"
        "LDR   R1, [R0]            \n"
        "LDR   R0, [R1]            \n"
        "LDMIA R0!, {R4-R11}       \n"
        "MSR   PSP, R0             \n"
        "MOVS  R0, #2              \n"
        "MSR   CONTROL, R0         \n"
        "ISB                       \n"
        "LDR   R2, =kernel_running \n"
        "MOVS  R3, #1              \n"
        "STR   R3, [R2]            \n"
        "LDR   LR, =0xFFFFFFFD     \n"      /* ← the missing line */
        "BX    LR                  \n"
    );
}

/*===========================================================================
 *                          Kernel Startup
 *===========================================================================*/

void kernel_start(void)
{
    if (taskCount == 0)
    {
        return;
    }

    currentTask = pick_highest_priority_ready_task();

    if (currentTask == 0)
    {
        for (;;)
        {
        }
    }

    currentTask->state = TASK_RUNNING;

    /*
     * PendSV must be the lowest-priority exception in the system.
     * Do NOT enable SysTick's reschedule requests here -- kernel_running
     * is still 0, and it must stay 0 until PSP is a valid task stack.
     */
    SCB_SHPR3 |= SHPR3_PENDSV_LOWEST;

    /* NOTE: kernel_running = 1; has been REMOVED from this function. */

    __asm volatile ("SVC #0");

    for (;;)
    {
    }
}
/*===========================================================================
 *                          SysTick Interface
 *===========================================================================*/

/*
 * Called once per SysTick interrupt (see SysTick_Handler in main.c).
 * Just requests a reschedule -- the actual switch is deferred until
 * PendSV's (lowest) priority allows it to run.
 */
void kernel_tick(void)
{
    kernel_request_reschedule();
}

/*===========================================================================
 *          Chapter 28: Generic Primitives for Synchronization Code
 *===========================================================================*/

/*
 * See kernel.h for the full rationale behind each of these. In
 * short: mutex.c needs to (a) know who is currently running, (b)
 * trigger a reschedule, (c) put a task it just unblocked back onto
 * the scheduler's books, (d) refuse to block from ISR context, and
 * (e) run a couple of instructions atomically -- and none of that
 * requires the scheduler to know mutexes exist.
 */

TCB_t *kernel_get_current_task(void)
{
    return currentTask;
}

void kernel_request_reschedule(void)
{
    SCB_ICSR = ICSR_PENDSVSET;
}

void kernel_wake_task(TCB_t *task)
{
    /*
     * Caller (mutex_unlock()) must have already removed 'task' from
     * whatever wait list it was on -- this function only knows how
     * to put a TCB back into the READY world, not how to take it out
     * of a synchronization primitive's private wait list.
     */
    task->state = TASK_READY;
    list_append(&readyList[task->priority - 1], task);

    /*
     * Reuse the exact same preemption rule kernel_create_task() uses
     * for "a task just became READY" -- there is only one such rule
     * in this kernel, regardless of what caused the task to become
     * READY.
     */
    kernel_request_preemption_if_needed(task);
}

uint8_t kernel_in_isr(void)
{
    /*
     * IPSR reads 0 in Thread mode (normal task execution) and holds
     * the active exception number whenever a handler (SysTick,
     * PendSV, USART2, EXTI, ...) is running. This is a direct
     * register read -- no CMSIS needed, consistent with how the rest
     * of this kernel talks to the CPU.
     */
    uint32_t ipsr;

    __asm volatile ("MRS %0, IPSR" : "=r" (ipsr));

    return (ipsr != 0) ? 1 : 0;
}

void kernel_enter_critical(void)
{
    /*
     * CPSID I sets PRIMASK, masking every exception whose priority
     * is configurable (everything except NMI and HardFault). This
     * is Chapter 28's "Mechanism A" -- see mutex.h for the full
     * theory discussion of why this is preferred here over
     * LDREX/STREX for a single-core kernel.
     */
    __asm volatile ("CPSID I" ::: "memory");
}

void kernel_exit_critical(void)
{
    __asm volatile ("CPSIE I" ::: "memory");
}

/*===========================================================================
 *                  SVC Handler - First Task Launch
 *===========================================================================*/

__attribute__((naked))
void SVC_Handler(void)
{
    __asm volatile ("B restore_context");
}

/*===========================================================================
 *                  PendSV Handler - Every Later Context Switch
 *===========================================================================*/

/*
 * Unchanged from Chapter 27. PendSV still never touches readyList[],
 * blockedList, or (new in this chapter) any Mutex_t's waitList --
 * only scheduler_pick_next() and restore_context() do, and PendSV
 * only calls the former and jumps into the latter.
 */
__attribute__((naked))
void PendSV_Handler(void)
{
    __asm volatile
    (
        "MRS   R0, PSP             \n"
        "STMDB R0!, {R4-R11}       \n"

        "LDR   R1, =currentTask    \n"
        "LDR   R2, [R1]            \n"
        "STR   R0, [R2]            \n"

        "BL    scheduler_pick_next \n"
        "B     restore_context     \n"
    );
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
