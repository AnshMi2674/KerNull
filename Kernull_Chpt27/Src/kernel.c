/******************************************************************************
 * File        : kernel.c
 * Project     : Educational RTOS
 * Chapter     : 27 - Fixed-Priority Preemptive Scheduling
 *
 * Description:
 *  Implements the kernel developed up to Chapter 27.
 *
 * Features (all preserved from Chapter 26):
 *  - Task creation
 *  - Fake stack initialization
 *  - Task startup via SVC (Artificial Exception Return)
 *  - Context switching via PendSV
 *  - Linked-list based task queues (list.h / list.c)
 *
 * New in Chapter 27:
 *  - readyList is no longer a single queue. It is now readyList[5],
 *    one queue per priority level. TASK_READY no longer means "in
 *    the ready queue" -- it means "in the ready queue for MY
 *    priority".
 *  - The scheduler always prefers the highest-priority non-empty
 *    ready queue, and only runs Round Robin among tasks that share a
 *    priority (see pick_highest_priority_ready_task() and
 *    scheduler_pick_next() below).
 *  - A small preemption-request helper compares a newly-READY task's
 *    priority against currentTask's priority and requests PendSV if
 *    the new task outranks whoever is currently running. Chapter 27
 *    has no blocking/wakeup events yet, so the only thing that can
 *    make a task newly READY is kernel_create_task() itself -- but
 *    the check is real and active, and is exactly what Chapter 28's
 *    taskDelay()/wakeup logic will reuse when a sleeping task wakes
 *    up higher-priority than whatever is currently running.
 *
 * What did NOT change
 * --------------------
 *  Every line of the SVC/PendSV assembly is unchanged from Chapter 26
 *  -- MRS/STMDB/LDMIA/MSR/BX LR, restore_context() shared by both
 *  handlers, PendSV containing zero scheduling logic. Priority is a
 *  scheduling POLICY change; it lives entirely in C, in
 *  scheduler_pick_next() and the small helper it now calls. The CPU
 *  context-switch mechanism does not know priority exists.
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
 * Unchanged from Chapter 26: taskList[] owns every TCB for its
 * entire lifetime. The ready queues below never hold a TCB that
 * isn't also a slot in this array -- they only borrow the 'next'
 * field that already lives inside each TCB_t.
 */
static TCB_t taskList[MAX_TASKS];

/*
 * Number of tasks currently created.
 */
static uint8_t taskCount = 0;

/*
 * Number of distinct priority levels the kernel supports.
 *
 * Derived from the public range in kernel.h so there is exactly one
 * place that defines "how many priorities exist" -- readyList's size
 * and every priority validation check below both come from this same
 * value.
 */
#define NUM_PRIORITIES (TASK_PRIORITY_MAX - TASK_PRIORITY_MIN + 1u)

/*
 * One READY queue per priority level.
 *
 * readyList[0] holds every READY task at priority 1 (highest).
 * readyList[NUM_PRIORITIES - 1] holds every READY task at priority 5
 * (lowest). A task's priority maps to its queue via
 * "readyList[priority - 1]" everywhere in this file.
 *
 * This is the entire mechanism behind fixed-priority scheduling: the
 * READY set is partitioned by priority up front, so "find the
 * highest-priority READY task" never requires scanning taskList[] or
 * inspecting every task's priority field -- it only requires checking
 * up to five list head pointers, in order, and taking the first
 * non-empty one. See pick_highest_priority_ready_task() below.
 */
static List_t readyList[NUM_PRIORITIES];

/*
 * Tasks waiting on something that hasn't happened yet.
 *
 * Not used by any code in this chapter -- nothing here ever moves a
 * task to TASK_BLOCKED. It is declared and initialized now, exactly
 * as in Chapter 26, so the chapter that introduces blocking only has
 * to add the state-transition logic, not the underlying storage.
 * Deliberately kept as a single list rather than one-per-priority:
 * ordering among blocked tasks is a different concern (usually "who
 * wakes up soonest", not "who is most important") and is out of
 * scope here.
 */
static List_t blockedList;

/*
 * Pointer to the currently RUNNING task's TCB.
 *
 * Unchanged in spirit from Chapter 26: this IS the pointer the
 * PendSV/SVC assembly dereferences, with no separate mirror variable.
 * The currently running task is deliberately NOT a member of any
 * ready list while it holds this position -- it only rejoins its
 * priority's ready queue (via list_append() inside
 * scheduler_pick_next()) at the moment it is switched OUT.
 *
 * Chapter 27 does NOT add a separate "currentPriority" variable.
 * currentTask->priority already answers that question, and keeping a
 * second copy would just be state that could drift out of sync with
 * the TCB for no benefit.
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
 *              Priority Preemption Request (Chapter 27)
 *===========================================================================*/

/*
 * Checks whether a task that just became READY should immediately
 * preempt whoever is currently running, and if so, requests PendSV.
 *
 * This function does NOT perform a context switch itself -- it only
 * pends PendSV, exactly like kernel_tick() does. The actual switch
 * still happens through the normal PendSV -> scheduler_pick_next() ->
 * restore_context() path, at whatever point PendSV's (lowest)
 * priority allows it to run. This keeps the same separation of
 * concerns the whole kernel has followed since Chapter 7: something
 * decides a switch SHOULD happen and requests PendSV; PendSV is the
 * only thing that ever actually performs one.
 *
 * Chapter 27 has exactly one caller for this: kernel_create_task().
 * A newly created task is the only way, in this chapter, for a task
 * to become READY after the scheduler may already be running. (In
 * this project's own main.c, every task is created before
 * kernel_start(), so currentTask is still NULL at that point and the
 * guard below makes this a safe no-op -- but the function is correct
 * for the general case.)
 *
 * Chapter 28 integration note:
 * When taskDelay()'s wakeup logic is added, waking a sleeping task is
 * exactly the same kind of event -- "a task just became READY that
 * wasn't a moment ago" -- and should call this same function after
 * moving the task out of the sleeping list and into its readyList[].
 * Nothing about this function is specific to task creation; it was
 * named and written generically for that reason.
 */
static void kernel_request_preemption_if_needed(TCB_t *readyTask)
{
    /*
     * If nothing is running yet (kernel_start() hasn't executed
     * SVC yet), there is nothing to preempt.
     */
    if (currentTask == 0)
    {
        return;
    }

    /*
     * Smaller priority number = more important (see kernel.h). If
     * the task that just became READY outranks the one currently
     * running, that task must not wait for the next SysTick tick --
     * it should run as soon as possible. Requesting PendSV here
     * achieves that without touching the CPU context directly from
     * this (non-exception) code path.
     */
    if (readyTask->priority < currentTask->priority)
    {
        SCB_ICSR = ICSR_PENDSVSET;
    }
}

/*===========================================================================
 *                          Task Creation
 *===========================================================================*/

/*
 * Creates a new task at a fixed priority and prepares its initial
 * execution context.
 *
 * A newly created task has never actually executed.
 *
 * Therefore, we manually construct the exact stack frame that the
 * Cortex-M processor expects after returning from an exception.
 *
 * From the processor's perspective, this new task appears identical
 * to a task that was previously interrupted.
 */
uint8_t kernel_create_task(void (*taskFunc)(void), const char *name, uint8_t priority)
{
    /*
     * Prevent creating more tasks than the kernel supports.
     */
    if (taskCount >= MAX_TASKS)
    {
        return 0;
    }

    /*
     * Priority is fixed for the task's entire lifetime and must fall
     * within the range this kernel actually has ready queues for.
     * Rejecting an out-of-range priority here, before anything else
     * is touched, keeps task creation all-or-nothing: either a fully
     * valid task is created, or nothing happens at all.
     */
    if (priority < TASK_PRIORITY_MIN || priority > TASK_PRIORITY_MAX)
    {
        return 0;
    }

    /*----------------------------------------------------------
     * Initialize Task Control Block
     *---------------------------------------------------------*/

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;
    taskList[taskCount].priority = priority;

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
     * The task becomes schedulable immediately, in the READY queue
     * for ITS priority -- not a single shared queue. This is the
     * only change from Chapter 26's "list_append(&readyList, ...)":
     * which queue a task joins is now a function of its priority.
     *
     * list_append() also takes care of setting this task's 'next'
     * pointer to 0 -- it doesn't need to be initialized separately
     * above.
     */
    list_append(&readyList[priority - 1], &taskList[taskCount]);

    /*
     * See the big comment on kernel_request_preemption_if_needed()
     * above for why this call is here and what it does. In this
     * project's own main.c, every task is created before
     * kernel_start() runs, so this is a safe no-op today -- but it
     * is the correct, general behaviour.
     */
    kernel_request_preemption_if_needed(&taskList[taskCount]);

    /*
     * One more task now exists in the system.
     */
    taskCount++;

    return 1;
}

/*===========================================================================
 *                  Fixed-Priority Round Robin Scheduler
 *===========================================================================*/

/*
 * Scans priority 1 through priority 5, in order, and pops the front
 * task off the first non-empty queue it finds.
 *
 * This is deliberately a plain linear scan across (at most) five list
 * head pointers -- not a bitmap, heap, or any other structure. With
 * only five priority levels, five comparisons in the worst case is
 * already about as fast as this can possibly be, and a bitmap/heap
 * would add real complexity for zero measurable benefit at this
 * scale. Simplicity that is easy to single-step in a debugger is a
 * design goal of this project, not an oversight.
 *
 * This is the ONE place in the kernel that knows how the five
 * priority queues are searched. Both kernel_start() (choosing the
 * very first task) and scheduler_pick_next() (choosing every task
 * after that) call this, so there is exactly one implementation of
 * "find the next task to run" anywhere in the kernel.
 *
 * Returns 0 (NULL) if every ready queue is empty. Chapter 27 has no
 * Idle Task yet (that's Chapter 28), so as long as at least one task
 * was ever created, this should never actually return 0 -- every
 * task is either READY in one of these queues or is currentTask
 * itself. Callers below treat a NULL result as a fatal kernel design
 * error rather than inventing an ad-hoc idle behaviour here.
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
 * Chapter 26 implemented plain Round Robin: whichever task has
 * waited longest gets to run next, full stop. Chapter 27 adds a
 * priority filter on top of that same idea:
 *
 *      1. If the outgoing task is still RUNNING (i.e. it simply used
 *         up its timeslice, rather than having been blocked by some
 *         other mechanism before this call), put it back to READY
 *         and append it to the back of ITS OWN priority's ready
 *         list -- readyList[currentTask->priority - 1]. This is what
 *         makes Round Robin happen WITHIN a priority level: a P2
 *         task that just ran goes to the back of the P2 queue, never
 *         into P1's or P3's.
 *      2. Ask pick_highest_priority_ready_task() for the next task.
 *         Because it always checks P1 before P2, P2 before P3, and
 *         so on, a lower-priority task can only ever be chosen when
 *         every higher-priority queue is completely empty. This is
 *         the entire mechanism behind strict priority: it isn't a
 *         special case anywhere, it falls straight out of always
 *         scanning queues in priority order.
 *
 * Why the RUNNING check in step 1 matters:
 * A task that has already been moved to TASK_BLOCKED by code outside
 * this function (a later chapter's semaphore/delay logic) has ALSO
 * already been removed from its ready list by that same code, via
 * list_remove(). If this function re-appended it anyway, a blocked
 * task would end up back in a READY queue and get CPU time it has no
 * business getting. Chapter 27 doesn't yet have any code that blocks
 * a task, so this branch is always taken today -- but writing it as
 * a check now, rather than an unconditional append, means nothing
 * here needs to change when blocking is introduced later.
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
        list_append(&readyList[currentTask->priority - 1], currentTask);
    }

    currentTask = pick_highest_priority_ready_task();

    if (currentTask == 0)
    {
        /*
         * Every ready queue was empty. In Chapter 27, with no Idle
         * Task, this can only mean a genuine kernel bug (e.g. a task
         * was removed from every list without a replacement ever
         * being queued) rather than a normal "nothing to do"
         * condition -- a normal system always has at least one
         * READY task at all times. Trapping here makes that bug loud
         * and immediate instead of silently jumping through a NULL
         * pointer a few lines below.
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
 * restore_context() loads whichever task currentTask points at and
 * performs the exception return into it.
 *
 * It is deliberately the ONLY place in the kernel that knows how to
 * finish a context switch. SVC_Handler jumps here to launch the first
 * task. PendSV_Handler jumps here after saving the outgoing task and
 * asking the scheduler for the next one. Same code, same guarantees,
 * every time -- the first task does not get special treatment, and
 * priority scheduling never had to change anything down here.
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
         * no struct-offset arithmetic needed in assembly.
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
 *      1. Ask pick_highest_priority_ready_task() for the first task
 *         to run. Because every task created so far is already
 *         sitting in its own readyList[priority - 1]
 *         (kernel_create_task() put it there), this naturally selects
 *         the highest-priority task that was created, and among
 *         equal-priority tasks, whichever was created first --
 *         exactly matching Chapter 26's "first created task", just
 *         now filtered by priority.
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
 * restore_context() (or, once a higher-priority task becomes READY,
 * by kernel_request_preemption_if_needed() requesting that same
 * PendSV early).
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

    currentTask = pick_highest_priority_ready_task();

    if (currentTask == 0)
    {
        /*
         * taskCount > 0 but every ready queue is empty -- should be
         * unreachable, since kernel_create_task() always appends
         * every task it creates to some readyList[]. Treated as a
         * fatal kernel bug rather than silently doing nothing.
         */
        for (;;)
        {
        }
    }

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
 *
 * Note that kernel_tick() itself carries no scheduling opinion at
 * all -- it does not know or care whether the resulting switch will
 * pick the same task again (as it will, every time, while a P1 task
 * remains READY and nothing else is) or a different one. That
 * decision belongs entirely to scheduler_pick_next(), as always.
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
 *      +-- scheduler_pick_next()   [re-queues outgoing task into its
 *      |                            OWN priority list if still RUNNING,
 *      |                            then picks the highest-priority
 *      |                            READY task and updates currentTask]
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
 * PendSV never touches readyList[]/blockedList directly, and never
 * looks at priority -- it only calls scheduler_pick_next(), which is
 * where all queue manipulation and priority policy live. PendSV's job
 * is exactly what it was in Chapter 26: save, ask the scheduler,
 * restore. Chapter 27 changed WHAT the scheduler decides, not what
 * PendSV does with that decision.
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
         * anything, exactly as in Chapter 26. scheduler_pick_next()
         * updates currentTask (and readyList[]) as a side effect.
         */
        "BL    scheduler_pick_next \n"

        /*
         * Load and restore whichever task scheduler_pick_next() just
         * selected, then perform the exception return. Shared with
         * SVC_Handler so a switch onto the first task (at boot) and a
         * switch onto any later task (mid-run, or via a priority
         * preemption request) go through identical code.
         */
        "B     restore_context     \n"
    );
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
