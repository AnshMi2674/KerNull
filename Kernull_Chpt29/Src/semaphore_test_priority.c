/******************************************************************************
 * File        : semaphore_tests_priority.c
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores
 *
 * Description:
 *  Live demonstrations of Tests 6-8 from the chapter spec. See
 *  semaphore_tests_basic.c's header for why these are real tasks
 *  under a live scheduler and why the full suite is split across
 *  two build modes.
 *
 *  This file does NOT define main(), SysTick_Handler(), or
 *  get_tick() -- all three live exactly once, in main.c (see
 *  app_config.h). This file supplies:
 *
 *    - task functions for Tests 6, 7, 8
 *    - semaphore_tests_priority_create_tasks(), called from main()
 *      when ACTIVE_APP_MODE == APP_MODE_TEST_PRIORITY
 *    - semaphore_test8_tick_hook(), called from main.c's
 *      SysTick_Handler (also only in this mode) to stand in for
 *      "a real interrupt fired," so Test 8 (ISR signaling) is
 *      observable without a serial terminal.
 *
 *  Exactly 8 tasks are created here -- the maximum this kernel's
 *  taskList[] supports (MAX_TASKS = 8).
 *
 *============================================================================
 *  DESIGN NOTES - READ BEFORE MODIFYING
 *============================================================================
 *
 *  1. THE testDoneSem TRICK
 *  ------------------------
 *  In a preemptive RTOS with no delay primitive, a task that
 *  finishes its work must NOT spin in `for(;;) { }`. That loop runs
 *  at the task's priority forever and starves every task with
 *  LOWER priority. (Chapter 28's version of main.c had exactly this
 *  bug -- the demo only worked when all three tasks were at the
 *  same priority.)
 *
 *  Fix: at the end of each test, block the task FOREVER on a
 *  semaphore nobody ever signals. The task goes TASK_BLOCKED,
 *  comes off every ready list, and stops consuming CPU. Later
 *  tests and lower-priority tasks then get to run.
 *
 *  testDoneSem is that "never signaled" semaphore.
 *
 *  2. WHY SIGNALER TASKS ARE AT P5, NOT P1
 *  ----------------------------------------
 *  It is tempting to make the signaler the highest priority so it
 *  runs first. That is WRONG for Tests 6 and 7.
 *
 *  These tests check what happens when a signal arrives while
 *  multiple tasks are already BLOCKED on the semaphore. If the
 *  signaler ran first, sem.count would just increment (no waiter
 *  yet), and whichever waiter eventually got scheduled would take
 *  the non-blocking path -- testing nothing.
 *
 *  With the signaler at P5 (lowest priority), it runs only after
 *  every P1-P4 task has had its turn (and, in these tests, has
 *  blocked on its semaphore). Then the signal arrives with the
 *  full set of waiters already queued, and the direct-handoff
 *  path is actually exercised.
 *
 *  3. WHY T6A IS AT P3, NOT P5
 *  ----------------------------
 *  Test 6 requires three tasks A (P3), B (P2), C (P4) to all be
 *  blocked on sem6 before the signaler runs. Blocking order on
 *  the wait list is determined by SCHEDULING order:
 *
 *      B (P2)  -> runs first (highest), blocks first
 *      A (P3)  -> runs next,              blocks second
 *      C (P4)  -> runs next,              blocks third
 *
 *  If A were at P5 like C, both would be BELOW the signaler and
 *  the signaler would run before either of them had blocked --
 *  the test would prove nothing. Putting A at P3 puts it above
 *  the P5 signaler, guaranteeing it blocks first.
 *
 *  The wait-list order then becomes: [B, A, C] (by scheduling
 *  order). When the signal fires, semaphore_signal() picks the
 *  SMALLEST priority number -> B (P2). That's the PASS condition.
 *
 *  4. Test 7's FAIRNESS CASE
 *  --------------------------
 *  D and E are both at P3. D is created first. On the wait list,
 *  D sits before E (FIFO within a priority). semaphore_signal()
 *  must pick D (first-queued), not E. This tests the
 *  "strictly-smaller-priority replacement" rule in
 *  semaphore_pick_highest_priority_waiter(): equal-priority
 *  candidates keep whichever was found first.
 *
 *  5. Test 8 AND THE TICK HOOK
 *  ----------------------------
 *  Test 8 is meant to prove semaphore_signal() is safe to call
 *  from interrupt context. Without a serial terminal feeding
 *  bytes, we simulate the interrupt by having SysTick_Handler
 *  call semaphore_test8_tick_hook() once, at tick 150. That hook
 *  then calls semaphore_signal() from ISR context -- exactly the
 *  code path an ISR would use.
 ******************************************************************************/

#include <stdint.h>
#include "kernel.h"
#include "semaphore.h"
#include "app_config.h"

#if ACTIVE_APP_MODE == APP_MODE_TEST_PRIORITY

extern void     usart2_send_string(const char *str);
extern uint32_t get_tick(void);

/* Busy-wait helper -- see note 3 in semaphore_tests_basic.c. */
static void delay_ticks(uint32_t ticks)
{
    uint32_t start = get_tick();
    while (get_tick() - start < ticks) { }
}

/*
 * "Park here forever" semaphore, shared by every test in this file.
 * See design note 1. Never signaled.
 */
static Semaphore_t testDoneSem;

/*===========================================================================
 * Test 6 - Priority-based waiter selection.
 *
 * Setup:   A (P3), B (P2), C (P4) all block on sem6.
 *          Signaler runs at P5, delays, then signals once.
 * Expect:  B wakes (highest priority among waiters).
 *          If A or C wakes, the priority selection logic is broken.
 *
 * Blocking order (by scheduler priority): B first, A next, C last.
 * Wait-list order: [B, A, C]. Signal picks B (smallest priority
 * number = highest priority).
 *===========================================================================*/
static Semaphore_t sem6;

static void test6_task_A(void) /* P3 -- see design note 3 */
{
    semaphore_wait(&sem6); /* blocks; will NOT be chosen */
    usart2_send_string("Test 6: FAIL - A (P3) woke instead of B\r\n");
    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test6_task_C(void) /* P4 -- blocks last */
{
    semaphore_wait(&sem6); /* blocks; will NOT be chosen */
    usart2_send_string("Test 6: FAIL - C (P4) woke instead of B\r\n");
    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test6_task_B(void) /* P2 -- blocks first, highest prio */
{
    semaphore_wait(&sem6); /* blocks; WILL be chosen */
    usart2_send_string("Test 6 (priority-based selection): B (P2) woke - PASS\r\n");
    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test6_signaler_task(void) /* P5 -- runs last, see note 2 */
{
    /* Give A, B, and C time to all reach semaphore_wait(&sem6)
     * and block. 80 ms is generous; they block within microseconds
     * of being first scheduled. */
    delay_ticks(80);

    usart2_send_string("Test 6: signaling once...\r\n");
    semaphore_signal(&sem6);

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Test 7 - Same-priority fairness.
 *
 * Setup:   D (P3), E (P3). D created first.
 *          Signaler runs at P5, delays, then signals once.
 * Expect:  D wakes (earliest-queued among equal priorities).
 *
 * D and E block in creation order, so the wait list is [D, E].
 * The "strictly-smaller-priority" replacement rule in
 * semaphore_pick_highest_priority_waiter() means the first-found
 * (D) survives any equal-priority comparison -- giving FIFO
 * fairness within the same priority.
 *===========================================================================*/
static Semaphore_t sem7;

static void test7_task_D(void) /* P3, blocks first */
{
    semaphore_wait(&sem7); /* blocks; WILL be chosen (first-queued) */
    usart2_send_string("Test 7 (same-priority fairness): D woke first - PASS\r\n");
    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test7_task_E(void) /* P3, blocks second */
{
    semaphore_wait(&sem7); /* blocks; will NOT be chosen */
    usart2_send_string("Test 7: FAIL - E woke before D\r\n");
    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test7_signaler_task(void) /* P5, runs after D and E block */
{
    delay_ticks(80);

    usart2_send_string("Test 7: signaling once...\r\n");
    semaphore_signal(&sem7);

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Test 8 - ISR signaling.
 *
 * Setup:   waiter (P1) blocks on sem8.
 *          SysTick_Handler calls semaphore_test8_tick_hook() every
 *          tick. At tick 150, the hook calls semaphore_signal(&sem8)
 *          from ISR context.
 * Expect:  waiter wakes and prints PASS.
 *
 * This is the ONLY test where the signaler is at higher priority
 * than the waiter -- because the signaler isn't a task at all, it's
 * the SysTick ISR itself, and ISRs always run at the priority the
 * NVIC gives them regardless of task priorities.
 *===========================================================================*/
static Semaphore_t sem8;

static void test8_waiter_task(void) /* P1 */
{
    usart2_send_string("Test 8: waiting for ISR signal...\r\n");

    /* Blocks here until the SysTick hook fires at tick 150. */
    semaphore_wait(&sem8);

    usart2_send_string("Test 8 (ISR signaling): woke after simulated ISR - PASS\r\n");

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*
 * Called from main.c's SysTick_Handler, under the same
 * ACTIVE_APP_MODE guard as this whole file, so it only exists (and
 * only runs) in this test mode.
 *
 * Demonstrates that semaphore_signal() is safe to call from
 * interrupt context. semaphore_wait() is NEVER called from this
 * hook -- that would still be illegal, since an ISR has no TCB to
 * block.
 *
 * The `fired` flag ensures this runs exactly once -- without it,
 * every tick after 150 would signal sem8, and the count would keep
 * climbing. With it, one signal at tick 150 wakes the waiter.
 */
void semaphore_test8_tick_hook(void)
{
    static uint8_t fired = 0;

    if (!fired && get_tick() >= 150)
    {
        fired = 1;
        semaphore_signal(&sem8); /* called from interrupt context */
    }
}

/*===========================================================================
 * Entry point called from main.c's main().
 *===========================================================================*/
void semaphore_tests_priority_create_tasks(void)
{
    usart2_send_string("=== Chapter 29 Semaphore Tests: Priority/ISR (6-8) ===\r\n");

    /* Must be initialized before any test can block on it. */
    semaphore_init(&testDoneSem, 0); /* never signaled */

    /* --- Test 6: priority-based waiter selection --- */
    semaphore_init(&sem6, 0);
    kernel_create_task(test6_task_A,        "T6A", 3); /* P3, see note 3 */
    kernel_create_task(test6_task_B,        "T6B", 2); /* P2, blocks first */
    kernel_create_task(test6_task_C,        "T6C", 4); /* P4, blocks last */
    kernel_create_task(test6_signaler_task, "T6S", 5); /* P5, see note 2 */

    /* --- Test 7: same-priority fairness --- */
    semaphore_init(&sem7, 0);
    kernel_create_task(test7_task_D,        "T7D", 3); /* P3, blocks first */
    kernel_create_task(test7_task_E,        "T7E", 3); /* P3, blocks second */
    kernel_create_task(test7_signaler_task, "T7S", 5); /* P5, see note 2 */

    /* --- Test 8: ISR signaling --- */
    semaphore_init(&sem8, 0);
    kernel_create_task(test8_waiter_task, "T8", 1);    /* P1, highest */

    /* 8 tasks total -- exactly MAX_TASKS. */
}

#endif /* APP_MODE_TEST_PRIORITY */
