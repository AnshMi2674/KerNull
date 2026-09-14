/******************************************************************************
 * File        : kernel.c
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores (atomicity correction pass)
 *
 * Description:
 *  Scheduler, PendSV/SVC context switching, and task creation are
 *  byte-for-byte unchanged from Chapter 28 -- see previous chapters
 *  for the full rationale behind every instruction there.
 *
 *  The only change in this file: kernel_wake_task() is now
 *  implemented on top of a new kernel_wake_task_locked(), which does
 *  the same READY+append+preempt-check work WITHOUT masking
 *  interrupts itself, so mutex.c and semaphore.c can call it from
 *  inside their OWN already-held critical section instead of opening
 *  a second, separate one after releasing the first -- see kernel.h
 *  for the full explanation of why that gap mattered and why this
 *  avoids nesting kernel_enter_critical()/kernel_exit_critical().
 ******************************************************************************/

#include <stdint.h>
#include "kernel.h"
#include "list.h"

/*===========================================================================
 *                      Core Register Addresses Used Here
 *===========================================================================*/

#define SCB_ICSR        (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET  (1u << 28)

#define SCB_SHPR3           (*(volatile uint32_t *)0xE000ED20)
#define SHPR3_PENDSV_LOWEST (0xFFu << 16)

/*===========================================================================
 *                          Kernel Private Data
 *===========================================================================*/

static TCB_t taskList[MAX_TASKS];
static uint8_t taskCount = 0;

#define NUM_PRIORITIES (TASK_PRIORITY_MAX - TASK_PRIORITY_MIN + 1u)

static List_t readyList[NUM_PRIORITIES];
static List_t blockedList;

volatile uint8_t kernel_running = 0;

static TCB_t *volatile currentTask __attribute__((used));

/*===========================================================================
 *                          Kernel Initialization
 *===========================================================================*/

void kernel_init(void)
{
    taskCount   = 0;
    currentTask = 0;

    for (uint8_t i = 0; i < NUM_PRIORITIES; i++)
    {
        list_init(&readyList[i]);
    }

    list_init(&blockedList);
}

/*===========================================================================
 *                          Task Exit Handler
 *===========================================================================*/

static void task_exit_error(void)
{
    while (1)
    {
        /* Future improvement: print task name, blink LED, breakpoint. */
    }
}

/*===========================================================================
 *              Priority Preemption Request
 *===========================================================================*/

/*
 * Checks whether a task that just became READY should immediately
 * preempt whoever is currently running, and if so, requests PendSV.
 *
 * Called from kernel_create_task() (a brand new task becoming READY)
 * and from kernel_wake_task_locked() (a mutex or semaphore waiter
 * becoming READY, whether reached via kernel_wake_task() or directly
 * from inside mutex.c/semaphore.c's own critical sections). One rule,
 * one implementation, regardless of which event triggered it.
 */
static void kernel_request_preemption_if_needed(TCB_t *readyTask)
{
    if (currentTask == 0)
    {
        return;
    }

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
    if (taskCount >= MAX_TASKS)
    {
        return 0;
    }

    if (priority < TASK_PRIORITY_MIN || priority > TASK_PRIORITY_MAX)
    {
        return 0;
    }

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;
    taskList[taskCount].priority = priority;

    uint32_t *sp = &taskList[taskCount].stack[STACK_SIZE];

    /*
     * Hardware exception frame.
     *
     * The CPU pops these in the order R0, R1, R2, R3, R12, LR, PC,
     * xPSR when it performs an exception return. Because the stack
     * grows downward, we must push them in REVERSE order: xPSR first
     * (highest address), R0 last (lowest -- SP ends up here).
     */
    *(--sp) = 0x01000000;                 /* xPSR - Thumb bit set */
    *(--sp) = (uint32_t)taskFunc;         /* PC   */
    *(--sp) = (uint32_t)task_exit_error;  /* LR   */
    *(--sp) = 0;                          /* R12  */
    *(--sp) = 0;                          /* R3   */
    *(--sp) = 0;                          /* R2   */
    *(--sp) = 0;                          /* R1   */
    *(--sp) = 0;                          /* R0   */

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
        for (;;)
        {
        }
    }

    currentTask->state = TASK_RUNNING;
}

/*===========================================================================
 *                  Shared Context Restore
 *===========================================================================*/

__attribute__((naked, used))
static void restore_context(void)
{
    __asm volatile
    (
        "LDR   R0, =currentTask        \n"
        "LDR   R1, [R0]                \n"
        "LDR   R0, [R1]                \n"

        "LDMIA R0!, {R4-R11}           \n"

        "MSR   PSP, R0                 \n"

        "MOVS  R0, #2                  \n"
        "MSR   CONTROL, R0             \n"
        "ISB                           \n"

        /*
         * PSP is now valid and CONTROL is set. This is the last safe
         * moment to arm SysTick-driven reschedules: from here on, any
         * SysTick -> PendSV chain will see a real task stack.
         */
        "LDR   R2, =kernel_running     \n"
        "MOVS  R3, #1                  \n"
        "STR   R3, [R2]                \n"

        "LDR   LR, =0xFFFFFFFD         \n"
        "BX    LR                      \n"
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

    SCB_SHPR3 |= SHPR3_PENDSV_LOWEST;

    __asm volatile ("SVC #0");

    for (;;)
    {
    }
}

/*===========================================================================
 *                          SysTick Interface
 *===========================================================================*/

void kernel_tick(void)
{
    kernel_request_reschedule();
}

/*===========================================================================
 *          Primitives for Synchronization Code
 *===========================================================================*/

TCB_t *kernel_get_current_task(void)
{
    return currentTask;
}

void kernel_request_reschedule(void)
{
    SCB_ICSR = ICSR_PENDSVSET;
}

/*
 * The raw, non-self-protecting operation. See kernel.h for the full
 * contract: callers MUST already hold an active kernel_enter_critical()
 * section. Exactly the same three steps Chapter 28's kernel_wake_task()
 * always did -- nothing about WHAT this does has changed, only where
 * the interrupt-masking boundary around it lives.
 */
void kernel_wake_task_locked(TCB_t *task)
{
    task->state = TASK_READY;
    list_append(&readyList[task->priority - 1], task);
    kernel_request_preemption_if_needed(task);
}

/*
 * Self-contained convenience wrapper: opens its own critical section,
 * does the wake, closes it. Correct and safe to call any time the
 * caller is NOT already inside a kernel_enter_critical() section.
 * Nothing in this codebase currently calls this version directly --
 * mutex.c and semaphore.c both call kernel_wake_task_locked() from
 * inside their own critical section that also covers removing the
 * waiter from their own wait list, per this pass's fix. This wrapper
 * is kept as the simple "just wake this task, don't make me think
 * about atomicity" entry point for any future caller with no
 * surrounding critical section of its own to fold this into.
 */
void kernel_wake_task(TCB_t *task)
{
    kernel_enter_critical();
    kernel_wake_task_locked(task);
    kernel_exit_critical();
}

uint8_t kernel_in_isr(void)
{
    uint32_t ipsr;

    __asm volatile ("MRS %0, IPSR" : "=r" (ipsr));

    return (ipsr != 0) ? 1 : 0;
}

void kernel_enter_critical(void)
{
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
