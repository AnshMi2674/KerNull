#include <stdint.h>
#include "kernel.h"

// Kernel data (private to kernel.c)
static TCB_t   taskList[MAX_TASKS];
static uint8_t taskCount   = 0;
static uint8_t currentTask = 0;


void kernel_init(void)
{
    taskCount   = 0;
    currentTask = 0;
}

static void task_exit_error(void)
{
    while (1)
    {
        // A task should never return.
        // Print that program returned even when it wasn't supposed to
    }
}

uint8_t kernel_create_task(void (*taskFunc)(void), const char *name)
{
    if (taskCount >= MAX_TASKS) {
        return 0;   // failure — process list is full
    }

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;

    uint32_t *sp;
    /*
     * Start with an empty stack.
     *
     * The Cortex-M stack grows toward lower addresses, so an empty
     * stack is represented by a pointer one element past the end
     * of the stack array.
     */
    sp = &taskList[taskCount].stack[STACK_SIZE];

    /*
     * Build the hardware exception frame.
     *
     * During Exception Return the Cortex-M automatically restores
     * these registers from the Process Stack Pointer (PSP).
     *
     * We create this frame manually so that a brand-new task looks
     * exactly like a task that was previously interrupted.
     */

    /*---------------- Hardware Exception Frame ----------------*/

    *(--sp) = 0;                            // R0
    *(--sp) = 0;                            // R1
    *(--sp) = 0;                            // R2
    *(--sp) = 0;                            // R3
    *(--sp) = 0;                            // R12
    *(--sp) = (uint32_t)task_exit_error;    // LR
    *(--sp) = (uint32_t)taskFunc;           // PC
    *(--sp) = 0x01000000;                   // xPSR (Thumb bit)

    /*
     * Build the software-saved frame.
     *
     * PendSV restores R4-R11 before performing Exception Return.
     * Therefore these registers must already exist on the stack,
     * even though the task has never executed before.
     */

    /*---------------- Software Saved Registers ----------------*/

    *(--sp) = 0;    // R11
    *(--sp) = 0;    // R10
    *(--sp) = 0;    // R9
    *(--sp) = 0;    // R8
    *(--sp) = 0;    // R7
    *(--sp) = 0;    // R6
    *(--sp) = 0;    // R5
    *(--sp) = 0;    // R4

    /*
     * Save the final PSP into the TCB.
     *
     * This pointer becomes the task's saved context. Whenever the
     * scheduler selects this task, PendSV will restore registers
     * beginning from this address.
     */
    taskList[taskCount].sp = sp;

    taskCount++;
    return 1;       // success
}

static void scheduler_pick_next(void)
{
    do
    {
        currentTask = (currentTask + 1) % taskCount;

    } while(taskList[currentTask].state != TASK_READY);
}

void kernel_start(void)
{
    if(taskCount == 0)
        return;

    currentTask = 0;

    /*
     * Chapter 6 is incomplete.
     *
     * Eventually this function will:
     *
     * 1. Switch Thread mode to PSP.
     * 2. Load PSP from taskList[0].sp.
     * 3. Restore the task context.
     * 4. Perform Exception Return.
     *
     * At that point the task begins executing at the
     * PC stored in its fake exception frame.
     */
}
