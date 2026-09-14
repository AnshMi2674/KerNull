/******************************************************************************
 * File        : kernel.c
 * Project     : Educational RTOS
 * Chapter     : 26 - Linked List Based Task Queues
 *
 * Description:
 *  Implements the kernel developed up to Chapter 26.
 *
 * Features (all preserved from Chapter 7):
 *  - Task creation
 *  - Fake stack initialization
 *  - Round Robin scheduler
 *  - Task startup via SVC (Artificial Exception Return)
 *  - Context switching via PendSV
 *
 * New in Chapter 26:
 *  - taskList[] is still where every TCB physically lives, but the
 *    scheduler no longer scans it. Instead, kernel.c keeps two
 *    List_t queues (readyList, blockedList) built from list.h/list.c,
 *    and currentTask is now a TCB_t* instead of an array index.
 *
 * What did NOT change
 * --------------------
 *  Every single line of the SVC/PendSV assembly logic is the same
 *  shape as Chapter 7 -- MRS/STMDB/LDMIA/MSR/BX LR, the
 *  restore_context() shared by both handlers, PendSV containing zero
 *  scheduling logic. The only difference at the assembly level is
 *  WHICH C symbol it dereferences to find "the current task's saved
 *  PSP" -- it was currentTCB_ptr in Chapter 7, and is simply
 *  currentTask now that currentTask itself is a TCB_t*. See
 *  restore_context() below for exactly how that trick still works.
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

/*===========================================================================
 *                          Kernel Private Data
 *===========================================================================*/

/*
 * Master list containing every task created in the system.
 *
 * This is unchanged from Chapter 7 and is exactly what design
 * decision #1 requires: taskList[] owns every TCB for its entire
 * lifetime. readyList/blockedList below never hold a TCB that isn't
 * also a slot in this array -- they only borrow the 'next' field
 * that already lives inside each TCB_t to describe an ordering over
 * these same structs.
 */
static TCB_t taskList[MAX_TASKS];

/*
 * Number of tasks currently created.
 */
static uint8_t taskCount = 0;

/*
 * Pointer to the currently RUNNING task's TCB.
 *
 * Chapter 7 used a uint8_t array index here, plus a separate
 * "currentTCB_ptr" mirror pointer purely so the PendSV/SVC assembly
 * had something pointer-shaped to dereference. Now that scheduling
 * itself is pointer-based, that indirection is gone: currentTask IS
 * the pointer the assembly needs, with no translation step.
 *
 * The currently running task is deliberately NOT a member of
 * readyList while it holds this position -- it only rejoins the
 * READY queue (via list_append() inside scheduler_pick_next()) at
 * the moment it is switched OUT. This keeps "which task is running"
 * and "which tasks are waiting" as two clearly separate concepts
 * instead of one list with a special "currently at the front but
 * also currently running" case to remember.
 *
 * volatile because it is written by C code (scheduler_pick_next,
 * kernel_start) and read back by asm handlers the compiler cannot
 * see into.
 *
 * __attribute__((used)): referenced only inside inline-asm string
 * literals, invisible to the compiler's normal dataflow analysis.
 * Without this, an aggressive optimizer could decide the variable is
 * "never read" and discard writes to it.
 */
static TCB_t *volatile currentTask __attribute__((used));

/*
 * The scheduler's two queues.
 *
 * List_t itself (see list.h) knows nothing about READY or BLOCKED --
 * it is kernel.c that decides these two specific lists exist and
 * what each one means. blockedList is not populated by anything in
 * this chapter (no code yet moves a task to TASK_BLOCKED), but it is
 * created and initialized now so that the chapter which introduces
 * blocking only has to add the state-transition logic, not the
 * underlying storage.
 */
static List_t readyList;
static List_t blockedList;

/*===========================================================================
 *                          Kernel Initialization
 *===========================================================================*/

/*
 * Initializes every kernel data structure.
 *
 * This function must be called once before creating any task.
 */
void kernel_init(void)
{
    taskCount   = 0;
    currentTask = 0;

    list_init(&readyList);
    list_init(&blockedList);
}

/*===========================================================================
 *                          Task Exit Handler
 *===========================================================================*/

/*
 * A task should NEVER return.
 *
 * If a task function reaches its closing brace, the processor will
 * automatically branch to the LR value stored in the fake exception
 * frame.
 *
 * During task creation we deliberately initialize LR with the address
 * of this function.
 *
 * Therefore, any task that accidentally returns will become trapped
 * here, making the programming error obvious.
 */
static void task_exit_error(void)
{
    while (1)
    {
        /*
         * Future improvement:
         *  - Print task name
         *  - Blink LED
         *  - Send UART message
         *  - Trigger breakpoint
         */
    }
}

/*===========================================================================
 *                          Task Creation
 *===========================================================================*/

/*
 * Creates a new task and prepares its initial execution context.
 *
 * A newly created task has never actually executed.
 *
 * Therefore, we manually construct the exact stack frame that the
 * Cortex-M processor expects after returning from an exception.
 *
 * From the processor's perspective, this new task appears identical
 * to a task that was previously interrupted.
 */
uint8_t kernel_create_task(void (*taskFunc)(void), const char *name)
{
    /*
     * Prevent creating more tasks than the kernel supports.
     */
    if (taskCount >= MAX_TASKS)
    {
        return 0;
    }

    /*----------------------------------------------------------
     * Initialize Task Control Block
     *---------------------------------------------------------*/

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;

    /*
     * Cortex-M stacks grow downward.
     *
     * Therefore an empty stack begins one word beyond the end of
     * the stack array.
     */
    uint32_t *sp = &taskList[taskCount].stack[STACK_SIZE];

    /*==========================================================
     *              Hardware Exception Stack Frame
     *==========================================================
     *
     * During an exception, Cortex-M hardware automatically pushes:
     *
     *      R0
     *      R1
     *      R2
     *      R3
     *      R12
     *      LR
     *      PC
     *      xPSR
     *
     * Since this task has never executed before, we fake that
     * stack frame manually.
     */

    *(--sp) = 0;                          /* R0   */
    *(--sp) = 0;                          /* R1   */
    *(--sp) = 0;                          /* R2   */
    *(--sp) = 0;                          /* R3   */
    *(--sp) = 0;                          /* R12  */

    /*
     * If the task ever returns,
     * execution jumps here.
     */
    *(--sp) = (uint32_t)task_exit_error;  /* LR   */

    /*
     * First instruction executed by the task.
     */
    *(--sp) = (uint32_t)taskFunc;         /* PC   */

    /*
     * Thumb bit must always remain set on Cortex-M.
     */
    *(--sp) = 0x01000000;                 /* xPSR */

    /*==========================================================
     *              Software Saved Registers
     *==========================================================
     *
     * Hardware does NOT save R4-R11.
     *
     * PendSV saves and restores these registers manually.
     *
     * Therefore they must already exist on the stack before the
     * task ever executes for the first time.
     */

    *(--sp) = 0;      /* R11 */
    *(--sp) = 0;      /* R10 */
    *(--sp) = 0;      /* R9  */
    *(--sp) = 0;      /* R8  */
    *(--sp) = 0;      /* R7  */
    *(--sp) = 0;      /* R6  */
    *(--sp) = 0;      /* R5  */
    *(--sp) = 0;      /* R4  */

    /*
     * Save the final Process Stack Pointer.
     *
     * Every task owns its own saved PSP.
     *
     * During a context switch:
     *
     *      Save PSP --> TCB
     *      Load PSP <-- TCB
     *
     * This allows each task to resume exactly where it stopped.
     */
    taskList[taskCount].sp = sp;

    /*
     * Chapter 26: the task becomes schedulable immediately, not
     * later inside kernel_start().
     *
     * This matters for more than just this chapter: a real RTOS lets
     * you call kernel_create_task() at any time, including after
     * kernel_start() has already handed control to some other task.
     * If joining the READY queue were postponed, a task created
     * "late" would silently never run. Appending here, unconditionally,
     * means kernel_create_task() has exactly one correct behaviour
     * regardless of when it's called.
     *
     * list_append() also takes care of setting this task's 'next'
     * pointer to 0 -- it doesn't need to be initialized separately
     * above.
     */
    list_append(&readyList, &taskList[taskCount]);

    /*
     * One more task now exists in the system.
     */
    taskCount++;

    return 1;
}

/*===========================================================================
 *                      Round Robin Scheduler
 *===========================================================================*/

/*
 * Selects the next READY task.
 *
 * Chapter 7 implemented Round Robin by scanning taskList[] forward
 * from the current index until it found a READY task. Chapter 26
 * implements the exact same policy -- tasks are served in the order
 * they became eligible, and a task that just ran goes to the back of
 * the line -- using a queue instead of a scan:
 *
 *      1. If the outgoing task is still RUNNING (i.e. it simply used
 *         up its timeslice, rather than having been blocked by some
 *         other mechanism before this call), put it back to READY
 *         and append it to the back of readyList.
 *      2. Pop whichever task has been waiting longest off the front
 *         of readyList. That is the next task to run.
 *
 * Why the RUNNING check in step 1 matters:
 * A task that has already been moved to TASK_BLOCKED by code outside
 * this function (a later chapter's semaphore/delay logic) has ALSO
 * already been removed from readyList by that same code, via
 * list_remove(). If this function re-appended it anyway, a blocked
 * task would end up back in the READY queue and get CPU time it has
 * no business getting. Chapter 26 doesn't yet have any code that
 * blocks a task, so this branch is always taken today -- but writing
 * it as a check now, rather than an unconditional append, means
 * nothing here needs to change when blocking is introduced later.
 *
 * __attribute__((used)): called only from inside PendSV_Handler's
 * inline-asm "BL scheduler_pick_next", which the compiler's dataflow
 * analysis cannot see. Without this, an optimizer could conclude this
 * static function is unreachable and discard it.
 */
static void scheduler_pick_next(void) __attribute__((used));
static void scheduler_pick_next(void)
{
    if (currentTask->state == TASK_RUNNING)
    {
        currentTask->state = TASK_READY;
        list_append(&readyList, currentTask);
    }

    /*
     * scheduler_pick_next() is only ever called from PendSV, which
     * only ever runs once kernel_start() has already put at least one
     * task into circulation. readyList is therefore guaranteed
     * non-empty here: this task itself was just appended above (or,
     * on a system with only one task, it's the only thing that was
     * ever in the list to begin with).
     */
    currentTask = list_pop_front(&readyList);
    currentTask->state = TASK_RUNNING;
}

/*===========================================================================
 *                  Shared Context Restore (used by both
 *                  SVC_Handler and PendSV_Handler)
 *===========================================================================*/

/*
 * restore_context() loads whichever task currentTask points at and
 * performs the exception return into it.
 *
 * It is deliberately the ONLY place in the kernel that knows how to
 * finish a context switch. SVC_Handler jumps here to launch the first
 * task. PendSV_Handler jumps here after saving the outgoing task and
 * asking the scheduler for the next one. Same code, same guarantees,
 * every time -- the first task does not get special treatment.
 *
 * naked: this function must contain nothing but the exact
 * instructions below. A normal (non-naked) function would have the
 * compiler generate its own prologue/epilogue (pushing/popping
 * registers, adjusting SP) which would corrupt the very stack pointer
 * we are in the middle of switching.
 *
 * __attribute__((used)): reached only via "B restore_context" inside
 * other naked functions' inline asm -- invisible to the compiler's
 * normal call-graph analysis, so it must be told explicitly not to
 * discard this as dead code.
 */
__attribute__((naked, used))
static void restore_context(void)
{
    __asm volatile
    (
        /*
         * currentTask holds the address of the incoming task's TCB.
         * Because 'sp' is the FIRST member of TCB_t, that same
         * address also holds the task's saved stack pointer -- so
         * one more load gets us the actual PSP value directly, with
         * no struct-offset arithmetic needed in assembly. This is
         * the exact same trick Chapter 7 used via currentTCB_ptr;
         * currentTask now plays that role directly since it is
         * itself a TCB_t*.
         */
        "LDR   R0, =currentTask    \n" /* R0 = &currentTask                */
        "LDR   R1, [R0]            \n" /* R1 = currentTask (== &TCB == &TCB.sp) */
        "LDR   R0, [R1]            \n" /* R0 = saved PSP for this task     */

        /*
         * Pop the software-saved half of the context back into the
         * physical registers. This is the exact mirror of the frame
         * kernel_create_task() pre-built (for a brand-new task) or
         * that PendSV's save half pushed (for a task resuming after
         * being switched out).
         */
        "LDMIA R0!, {R4-R11}       \n"

        /*
         * R0 now points just past R4-R11, i.e. at the top of the
         * hardware exception frame (R0-R3, R12, LR, PC, xPSR) that
         * is still sitting on this task's stack. Telling the CPU
         * "this is your Process Stack Pointer now" is what lets the
         * upcoming exception return unstack that hardware frame from
         * the RIGHT task's stack instead of whatever MSP/PSP was in
         * use before.
         */
        "MSR   PSP, R0             \n"

        /*
         * CONTROL[1] (SPSEL) = 1 tells the CPU "use PSP, not MSP,
         * while in Thread mode". Every task runs in Thread mode using
         * PSP; only exception handlers use MSP. This bit only
         * actually needs setting once (on the very first launch --
         * afterwards the CPU is already in Thread+PSP whenever we get
         * back here), but re-setting it on every switch is harmless
         * and keeps this routine identical no matter who called it.
         * ISB flushes the pipeline so the very next instruction
         * fetch/return honors the new CONTROL value.
         */
        "MOVS  R0, #2              \n"
        "MSR   CONTROL, R0         \n"
        "ISB                       \n"

        /*
         * EXC_RETURN value: 0xFFFFFFFD means "return to Thread mode,
         * use PSP, no floating-point state". Loading this into LR and
         * branching to it is what makes BX LR an exception return
         * instead of an ordinary branch (this only works because we
         * are executing inside a real exception -- SVC or PendSV --
         * which is the whole reason kernel_start() had to raise SVC
         * rather than jumping here directly from Thread mode).
         *
         * We force this value explicitly rather than trusting
         * whatever EXC_RETURN hardware already put in LR at handler
         * entry, because on the very first launch the CPU entered SVC
         * from Thread+MSP (LR would say "return using MSP") -- but we
         * are switching to PSP for the first time right here. Forcing
         * 0xFFFFFFFD makes this routine correct for both the first
         * launch and every later PendSV switch, uniformly.
         */
        "LDR   LR, =0xFFFFFFFD     \n"
        "BX    LR                 \n"
    );
}

/*===========================================================================
 *                          Kernel Startup
 *===========================================================================*/

/*
 * Starts the RTOS.
 *
 * This function executes exactly once and never returns to its
 * caller (main()).
 *
 * Sequence:
 *      1. Pop the first task off the front of readyList and mark it
 *         RUNNING. Since kernel_create_task() appends in creation
 *         order, this is still "the first task that was created" --
 *         exactly matching Chapter 7's currentTask = 0 -- just
 *         reached via the queue instead of an index.
 *      2. Configure PendSV to run at the lowest priority in the
 *         system, so it can never preempt another interrupt mid-way.
 *      3. Raise SVC. This is the ONLY way to legally reach an
 *         exception-return-capable Handler mode from here (see the
 *         file-level comment at the top of this file for why BX LR
 *         cannot be used directly from Thread mode).
 *      4. SVC_Handler jumps into restore_context(), which loads the
 *         first task's context and performs the actual exception
 *         return.
 *
 * Every future context switch after this point is handled entirely
 * by SysTick -> kernel_tick() -> PendSV -> scheduler_pick_next() ->
 * restore_context().
 */
void kernel_start(void)
{
    /*
     * Safety check.
     *
     * Starting an RTOS without any tasks is almost certainly
     * a programming error.
     */
    if (taskCount == 0)
    {
        return;
    }

    /*
     * Every task created so far is already sitting in readyList
     * (kernel_create_task() put it there). Popping the front gives
     * us the first task, in creation order, and simultaneously
     * removes it from the queue -- exactly the state a RUNNING task
     * is supposed to be in (see the currentTask comment above).
     */
    currentTask = list_pop_front(&readyList);
    currentTask->state = TASK_RUNNING;

    /*
     * PendSV must be the lowest-priority exception in the system.
     * This single line is what guarantees a context switch always
     * waits for USART2/EXTI/SysTick handlers to fully finish first.
     */
    SCB_SHPR3 |= SHPR3_PENDSV_LOWEST;

    /*
     * Raise SVC to reach Handler mode, from which an artificial
     * exception return into the first task is legal. Control does
     * not return here -- restore_context() ends with BX LR, landing
     * directly in that task.
     */
    __asm volatile ("SVC #0");

    /*
     * Unreachable. Kept only as a defensive trap in case SVC_Handler
     * is ever misconfigured (e.g. wrong vector table entry), so a
     * mistake here fails loudly instead of falling back into main().
     */
    for (;;)
    {
    }
}

/*===========================================================================
 *                          SysTick Interface
 *===========================================================================*/

/*
 * Called once per SysTick interrupt (see SysTick_Handler in main.c).
 *
 * Deliberately does the absolute minimum: request PendSV and return.
 * The actual context switch is deferred until PendSV's turn comes up,
 * which -- because of the priority set in kernel_start() -- is
 * guaranteed to be after every other pending interrupt has finished.
 */
void kernel_tick(void)
{
    SCB_ICSR = ICSR_PENDSVSET;
}

/*===========================================================================
 *                  SVC Handler - First Task Launch
 *===========================================================================*/

/*
 * Entered once, synchronously, the instant kernel_start() executes
 * "SVC #0". Being a real exception, this puts the CPU in Handler
 * mode -- which is the only place an artificial exception return is
 * legal. All the actual work is delegated to restore_context().
 *
 * naked: must not have a compiler-generated prologue/epilogue, for
 * the same reason restore_context() must not.
 */
__attribute__((naked))
void SVC_Handler(void)
{
    __asm volatile ("B restore_context");
}

/*===========================================================================
 *                  PendSV Handler - Every Later Context Switch
 *===========================================================================*/

/*
 * PendSV performs every context switch after the first.
 *
 * Context Switch Timeline
 * -----------------------
 *
 * Running Task
 *      |
 *      v
 * Hardware saves (automatically, on exception entry):
 *      R0-R3, R12, LR, PC, xPSR
 *      |
 *      v
 * PendSV_Handler
 *      |
 *      +-- Save R4-R11 (software half)
 *      +-- Save updated PSP into outgoing task's TCB
 *      +-- scheduler_pick_next()   [re-queues outgoing task, pops next
 *      |                            one off readyList, updates currentTask]
 *      +-- restore_context():
 *              Load next task's saved PSP
 *              Restore R4-R11
 *              Update PSP register
 *              BX LR (artificial exception return)
 *      |
 *      v
 * Hardware restores (automatically, on exception return):
 *      R0-R3, R12, LR, PC, xPSR
 *      |
 *      v
 * Next Task Running
 *
 * Notice this handler never touches readyList/blockedList directly --
 * it only calls scheduler_pick_next(), which is where all queue
 * manipulation happens. PendSV's only job is save-schedule-restore,
 * exactly as required.
 *
 * naked: same reasoning as restore_context() -- this function's
 * entire body IS the context switch. A compiler-inserted
 * prologue/epilogue would push/pop registers using whatever stack is
 * currently active, corrupting the exact mechanism this handler
 * implements.
 */
__attribute__((naked))
void PendSV_Handler(void)
{
    __asm volatile
    (
        /*
         * PSP currently points at the top of the outgoing task's
         * stack, just above the hardware exception frame that the
         * CPU already pushed automatically on entry to this handler.
         */
        "MRS   R0, PSP             \n"

        /*
         * Push the software-managed half of the context (R4-R11).
         * STMDB = "store multiple, decrement before": pushes
         * downward, exactly matching how the hardware frame below it
         * was pushed, so the two halves sit contiguously on the
         * task's own stack.
         */
        "STMDB R0!, {R4-R11}       \n"

        /*
         * Save the updated stack pointer into the outgoing task's
         * TCB. Same trick as in restore_context(): currentTask
         * points at the TCB, which (since 'sp' is the first member)
         * is the same address as the TCB's 'sp' field.
         */
        "LDR   R1, =currentTask    \n"
        "LDR   R2, [R1]            \n"
        "STR   R0, [R2]            \n"

        /*
         * Ask the scheduler which task runs next. This is the ONLY
         * place scheduling logic runs -- PendSV itself never decides
         * anything, exactly as design decision #2/#10 requires.
         * scheduler_pick_next() updates currentTask (and readyList)
         * as a side effect.
         */
        "BL    scheduler_pick_next \n"

        /*
         * Load and restore whichever task scheduler_pick_next() just
         * selected, then perform the exception return. Shared with
         * SVC_Handler so a switch onto the first task (at boot) and a
         * switch onto any later task (mid-run) go through identical
         * code.
         */
        "B     restore_context     \n"
    );
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
