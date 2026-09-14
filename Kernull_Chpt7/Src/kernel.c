/******************************************************************************
 * File        : kernel.c
 * Project     : Educational RTOS
 * Chapter     : 7 - Context Switching and Round Robin Scheduler
 *
 * Description:
 *  Implements the kernel developed up to Chapter 7.
 *
 * Features:
 *  - Task creation
 *  - Fake stack initialization
 *  - Round Robin scheduler
 *  - Real task startup via SVC (Artificial Exception Return)
 *  - Real context switching via PendSV
 *
 * Why SVC is needed to start the first task
 * ------------------------------------------
 *  BX LR only performs an exception return when the CPU is currently
 *  in Handler mode (i.e. already executing an exception). kernel_start()
 *  is called from main() in Thread mode, so it cannot simply load a
 *  context and BX LR -- the CPU would treat 0xFFFFFFFD as a normal
 *  branch target and fault.
 *
 *  The fix: kernel_start() executes "SVC #0". This raises a real
 *  exception, which puts the CPU into Handler mode. From inside
 *  SVC_Handler, BX LR with an EXC_RETURN value is legal and performs
 *  a genuine exception return -- landing us in Task0 exactly the way
 *  every future PendSV switch will land us in whichever task is next.
 *
 *  Both SVC_Handler (first launch) and PendSV_Handler (every switch
 *  after that) end by jumping into the same restore_context(), so the
 *  first task starts through the identical code path as every later
 *  one -- there is no special case for Task0 at the CPU level.
 ******************************************************************************/

#include <stdint.h>
#include "kernel.h"

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
 * Future chapters will introduce READY and BLOCKED lists, but every
 * task will always have exactly one TCB stored in this array.
 */
static TCB_t taskList[MAX_TASKS];

/*
 * Number of tasks currently created.
 */
static uint8_t taskCount = 0;

/*
 * Index of the task currently considered "running".
 *
 * Chapter 7 uses simple array indices as the scheduling data
 * structure. Future chapters may replace this with pointers/lists.
 */
static uint8_t currentTask = 0;

/*
 * Pointer to the currently running task's TCB.
 *
 * This is NOT a replacement for currentTask -- it is a convenience
 * mirror of &taskList[currentTask], kept purely so the assembly in
 * SVC_Handler / PendSV_Handler / restore_context can reach the saved
 * PSP with a single load instead of computing "taskList + currentTask
 * * sizeof(TCB_t)" by hand in assembly. All scheduling decisions are
 * still made in C, using the index, exactly as your design intended.
 *
 * volatile because it is written by C code and read back by asm
 * handlers that the compiler cannot see into.
 *
 * __attribute__((used)): referenced only inside inline-asm string
 * literals, which the compiler's dataflow analysis can't see. Without
 * this, an aggressive optimizer could decide the variable is "never
 * read" and discard writes to it.
 */
static TCB_t *volatile currentTCB_ptr __attribute__((used));

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
    taskCount      = 0;
    currentTask    = 0;
    currentTCB_ptr = 0;
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
 * Chapter 7 uses a simple Round Robin scheduler.
 *
 * Starting from the currently running task, the scheduler scans
 * forward until it finds another READY task.
 *
 * Also responsible for keeping TaskState honest: the task being
 * switched out goes back to READY, and the task being switched in
 * becomes RUNNING. (Previously TASK_RUNNING was declared but never
 * actually assigned anywhere in the kernel -- this was a genuine
 * gap, not a stylistic choice, so it's fixed here rather than left
 * as-is.)
 *
 * __attribute__((used)): called only from inside PendSV_Handler's
 * inline-asm "BL scheduler_pick_next", which the compiler's dataflow
 * analysis cannot see. Without this, an optimizer could conclude this
 * static function is unreachable and discard it.
 */
static void scheduler_pick_next(void) __attribute__((used));
static void scheduler_pick_next(void)
{
    taskList[currentTask].state = TASK_READY;

    /*
     * Continue searching until a READY task is found.
     */
    do
    {
        currentTask = (currentTask + 1) % taskCount;

    } while (taskList[currentTask].state != TASK_READY);

    taskList[currentTask].state = TASK_RUNNING;
    currentTCB_ptr = &taskList[currentTask];
}

/*===========================================================================
 *                  Shared Context Restore (used by both
 *                  SVC_Handler and PendSV_Handler)
 *===========================================================================*/

/*
 * restore_context() loads whichever task currentTCB_ptr points at and
 * performs the exception return into it.
 *
 * It is deliberately the ONLY place in the kernel that knows how to
 * finish a context switch. SVC_Handler jumps here to launch Task0.
 * PendSV_Handler jumps here after saving the outgoing task and asking
 * the scheduler for the next one. Same code, same guarantees, every
 * time -- Task0 does not get special treatment.
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
         * currentTCB_ptr holds the address of the incoming task's
         * TCB. Because 'sp' is the FIRST member of TCB_t, that same
         * address also holds the task's saved stack pointer -- so
         * one more load gets us the actual PSP value directly,
         * with no struct-offset arithmetic needed in assembly.
         */
        "LDR   R0, =currentTCB_ptr \n" /* R0 = &currentTCB_ptr            */
        "LDR   R1, [R0]            \n" /* R1 = currentTCB_ptr (== &TCB == &TCB.sp) */
        "LDR   R0, [R1]            \n" /* R0 = saved PSP for this task    */

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
 *      1. Select the first task (currentTask = 0) and mark it RUNNING.
 *      2. Configure PendSV to run at the lowest priority in the
 *         system, so it can never preempt another interrupt mid-way.
 *      3. Raise SVC. This is the ONLY way to legally reach an
 *         exception-return-capable Handler mode from here (see the
 *         file-level comment at the top of this file for why BX LR
 *         cannot be used directly from Thread mode).
 *      4. SVC_Handler jumps into restore_context(), which loads
 *         Task0's context and performs the actual exception return.
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
     * Chapter 7 uses the first created task as the initial task.
     * Later chapters may ask the scheduler to decide this instead.
     */
    currentTask = 0;
    taskList[currentTask].state = TASK_RUNNING;
    currentTCB_ptr = &taskList[currentTask];

    /*
     * PendSV must be the lowest-priority exception in the system.
     * This single line is what guarantees a context switch always
     * waits for USART2/EXTI/SysTick handlers to fully finish first.
     */
    SCB_SHPR3 |= SHPR3_PENDSV_LOWEST;

    /*
     * Raise SVC to reach Handler mode, from which an artificial
     * exception return into Task0 is legal. Control does not return
     * here -- restore_context() ends with BX LR, landing directly in
     * Task0.
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
 *      +-- scheduler_pick_next()   [may change currentTask/currentTCB_ptr]
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
         * TCB. Same trick as in restore_context(): currentTCB_ptr
         * points at the TCB, which (since 'sp' is the first member)
         * is the same address as the TCB's 'sp' field.
         */
        "LDR   R1, =currentTCB_ptr \n"
        "LDR   R2, [R1]            \n"
        "STR   R0, [R2]            \n"

        /*
         * Ask the scheduler which task runs next. This is the ONLY
         * place scheduling logic runs -- PendSV itself never decides
         * anything, exactly as your design decision #2 requires.
         * scheduler_pick_next() updates currentTask and
         * currentTCB_ptr as a side effect.
         */
        "BL    scheduler_pick_next \n"

        /*
         * Load and restore whichever task scheduler_pick_next() just
         * selected, then perform the exception return. Shared with
         * SVC_Handler so a switch onto Task0 (at boot) and a switch
         * onto Task2 (mid-run) go through identical code.
         */
        "B     restore_context     \n"
    );
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
