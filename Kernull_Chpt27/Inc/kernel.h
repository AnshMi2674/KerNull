/******************************************************************************
 * File        : kernel.h
 * Project     : Educational RTOS
 * Chapter     : 27 - Fixed-Priority Preemptive Scheduling
 *
 * Description:
 *  Public interface for the RTOS kernel developed up to Chapter 27.
 *
 *  This version supports everything Chapter 26 supported:
 *      - Task creation
 *      - Task Control Blocks (TCBs)
 *      - Fake stack initialization
 *      - Real context switching (SVC for first launch, PendSV for every
 *        switch after that)
 *      - Linked-list based task queues (see list.h)
 *
 *  Chapter 27 changes ONLY the scheduling policy: tasks now carry a
 *  fixed priority assigned at creation time, and the scheduler always
 *  prefers the highest-priority READY task, running Round Robin only
 *  among tasks that share the same priority. Every context-switching
 *  mechanism from Chapter 26 -- the SVC bootstrap, the artificial
 *  exception return, the PendSV save/restore sequence, PendSV being
 *  scheduling-logic-free -- is unchanged.
 *
 * Notes:
 *  - Context switching is performed using PendSV.
 *  - The first task is started using an Artificial Exception Return,
 *    reached via SVC (kernel_start() cannot BX LR into Handler mode
 *    directly -- see kernel.c for why).
 *  - Delays, an Idle Task, mutexes and semaphores are not part of
 *    this chapter and will be added later.
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
 * the LOWEST. Smaller number = more important, which matches how
 * most real RTOSes (and interrupt priority registers, for that
 * matter) express "priority" -- so this convention is deliberately
 * kept consistent with the PendSV/SysTick priority values already
 * used elsewhere in this kernel.
 *
 * There are exactly five levels because Chapter 27 asks for exactly
 * five. This is not meant to scale arbitrarily -- kernel.c uses one
 * readyList per level, so growing this range means growing that
 * array too. That is an intentional, simple design for an
 * educational kernel with a small, fixed number of priorities.
 */
#define TASK_PRIORITY_MIN   1u
#define TASK_PRIORITY_MAX   5u

/*===========================================================================
 *                              Task States
 *===========================================================================*/

/*
 * Every task exists in exactly one state.
 *
 * As of Chapter 27, READY and RUNNING are both actively used and
 * actively kept in sync with which list a task lives on:
 *      TASK_READY   -> the task is sitting in kernel.c's
 *                       readyList[priority - 1], waiting for its turn.
 *      TASK_RUNNING -> the task is currently executing, and is NOT
 *                       in any list (see currentTask in kernel.c).
 * BLOCKED is included because it becomes necessary in a later
 * chapter, once tasks can wait on something (a delay, a semaphore).
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
     * Examples:
     *  - waiting for a semaphore
     *  - waiting for a delay
     *  - waiting for I/O
     *
     * Used in later chapters.
     */
    TASK_BLOCKED

} TaskState;

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
     * This points to the top of the task's stack
     * after PendSV has saved R4-R11.
     *
     * Every task has its own saved PSP because
     * the Cortex-M CPU contains only one physical
     * PSP register.
     *
     * IMPORTANT: this must remain the FIRST member of TCB_t.
     * The PendSV/SVC assembly takes advantage of the fact that a
     * pointer to a TCB_t and a pointer to its 'sp' field are the
     * same address, which lets the handlers reach the saved stack
     * pointer with a plain load instead of computing a struct offset
     * in assembly. Chapter 27 adds a new field below, but does not
     * touch this layout guarantee.
     */
    uint32_t *sp;

    /*
     * Private stack belonging exclusively to this task.
     *
     * The stack stores:
     *
     *  - Hardware exception frame
     *  - Software-saved registers
     *  - Local variables
     *  - Function call frames
     *
     * Aligned to 8 bytes because the Cortex-M exception entry/exit
     * sequence expects (and by default enforces via the STKALIGN
     * feature) an 8-byte aligned stack pointer at the point an
     * exception frame is pushed/popped.
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
     * "Intrusive" means the link lives inside the node itself instead
     * of in a separate wrapper node -- there is no extra allocation
     * involved in putting a TCB on a list, which matters because this
     * kernel never allocates memory dynamically. This single 'next'
     * field is what lets any TCB_t sit on readyList[N], blockedList,
     * or any future queue, without list.c ever needing to know what a
     * TCB actually is beyond "something with a next pointer".
     *
     * A given TCB is on at most one list at a time. The currently
     * RUNNING task is on NO list -- it is only ever referenced via
     * the kernel's currentTask pointer (see kernel.c) -- which is why
     * TASK_RUNNING and "being in a ready list" are mutually exclusive.
     */
    struct TCB_t *next;

    /*
     * Fixed scheduling priority (Chapter 27).
     *
     * Valid range: TASK_PRIORITY_MIN (1, highest) to
     * TASK_PRIORITY_MAX (5, lowest). Assigned once, at
     * kernel_create_task() time, and never changed afterward --
     * Chapter 27 deliberately provides no API to change a task's
     * priority at runtime. This single field is also what
     * readyList[priority - 1] in kernel.c is indexed by, and what
     * lets the scheduler and the preemption check compare "is this
     * other task more important than me" with a plain integer
     * comparison instead of any separate priority bookkeeping.
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
 * As of Chapter 27, task creation requires a priority in the range
 * [TASK_PRIORITY_MIN, TASK_PRIORITY_MAX]. The task is immediately
 * appended to the READY queue for that priority -- it does not wait
 * until kernel_start() to become schedulable.
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
 * This function never returns.
 *
 * Responsibilities:
 *
 *  - Select the first task: the highest-priority READY task (and,
 *    among equal-priority tasks, whichever was created first).
 *  - Trigger an SVC exception, which performs the artificial
 *    exception return into that task.
 *
 * Once this function starts the first task,
 * all future task switches occur through PendSV.
 */
void kernel_start(void);

/*
 * Called once per SysTick interrupt (from SysTick_Handler in main.c).
 *
 * kernel_tick() does NOT perform the context switch itself. It only
 * marks PendSV as pending. The actual switch happens later, once the
 * CPU returns from SysTick_Handler and no higher-priority exception
 * is active -- because PendSV is deliberately configured at the
 * lowest priority in the system. This guarantees a context switch
 * never interrupts another ISR halfway through.
 */
void kernel_tick(void);

#endif /* KERNEL_H */
