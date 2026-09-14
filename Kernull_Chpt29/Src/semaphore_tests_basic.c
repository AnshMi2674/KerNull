/******************************************************************************
 * File        : semaphore_tests_basic.c
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores
 *
 * Description:
 *  Live demonstrations of Tests 1-5 from the chapter spec, as real
 *  tasks running under the actual scheduler.
 *
 *  Why real tasks and not a pre-boot unit-test harness:
 *  semaphore_wait()'s blocking path calls kernel_get_current_task()
 *  and dereferences it (`self->state = TASK_BLOCKED`). Before
 *  kernel_start() has run, currentTask is NULL. So there is no safe
 *  way to exercise the actual blocking path without a live scheduler.
 *  Every behaviour in this project is demonstrated the same way:
 *  real tasks, real preemption, output read off the UART log.
 *
 *  This file does NOT define main(), SysTick_Handler(), or
 *  get_tick() -- all three live exactly once, in main.c (see
 *  app_config.h). This file only supplies task functions plus one
 *  entry point, semaphore_tests_basic_create_tasks(), that main()
 *  calls when ACTIVE_APP_MODE == APP_MODE_TEST_BASIC.
 *
 *  See semaphore_tests_priority.c for Tests 6-8 (a separate build
 *  mode -- 5 + 8 = 13 tasks would exceed MAX_TASKS = 8 if both ran
 *  at once).
 *
 *============================================================================
 *  DESIGN NOTES - READ BEFORE MODIFYING
 *============================================================================
 *
 *  1. THE testDoneSem TRICK
 *  ------------------------
 *  In a preemptive RTOS with no delay primitive, a task that
 *  finishes its work must NOT simply spin in a `for(;;) { }` loop.
 *  That loop runs at the task's priority forever and starves every
 *  task with LOWER priority. (This was a real bug in the Chapter 28
 *  version of this project -- see the "Chapter 28 starvation"
 *  comment in main.c.)
 *
 *  The fix: at the end of each test's work, block the task
 *  FOREVER on a semaphore nobody ever signals. The task goes
 *  TASK_BLOCKED, comes off every ready list, and stops consuming
 *  CPU. Later tests (and lower-priority tasks) get to run.
 *
 *  testDoneSem is that "never signaled" semaphore. It is a
 *  deliberate misuse of the semaphore API as a "park here forever"
 *  primitive, and that's fine -- semaphore_wait() doesn't know or
 *  care what the caller intends.
 *
 *  2. WHY THE SIGNALER IS LOWER PRIORITY THAN THE WAITER
 *  ------------------------------------------------------
 *  Tests 2 and 3 need the waiter to have BLOCKED on sem23 BEFORE
 *  the signaler runs. If the signaler were higher priority, it
 *  would run first, increment sem23.count to 1, and the waiter
 *  would then find count > 0 and take the non-blocking path --
 *  proving nothing about the blocking path.
 *
 *  Solution: signaler is at P4 (lower than the waiter's P3).
 *  The waiter runs first, blocks. Only then does the signaler get
 *  scheduled, and it signals the already-blocked waiter.
 *
 *  3. delay_ticks() -- WHY NOT kernel_delay()?
 *  --------------------------------------------
 *  This kernel has no delay primitive yet (it's a future chapter).
 *  delay_ticks() is a busy-wait on tick_count -- crude, but
 *  deterministic and sufficient for a demonstration. It runs BEFORE
 *  the calling task blocks, so its only cost is a few ms of CPU on
 *  a task that would otherwise be spinning anyway.
 ******************************************************************************/

#include <stdint.h>
#include "kernel.h"
#include "semaphore.h"
#include "app_config.h"

#if ACTIVE_APP_MODE == APP_MODE_TEST_BASIC

/* Provided by main.c -- see its comments for what these do. */
extern void     usart2_send_string(const char *str);
extern uint32_t get_tick(void);

/*
 * Busy-wait for the given number of SysTick ticks (1 tick = 1 ms at
 * the project's configured SYST_RVR). See design note 3 above for
 * why we use this instead of a kernel primitive.
 */
static void delay_ticks(uint32_t ticks)
{
    uint32_t start = get_tick();
    while (get_tick() - start < ticks) { }
}

/*
 * "Park here forever" semaphore. Never signaled by any code in this
 * file. See design note 1 at the top of this file.
 */
static Semaphore_t testDoneSem;

/*===========================================================================
 * Test 1 - Initial token, wait does not block.
 *
 * Setup:   sem1 initialized with count = 1.
 * Action:  one semaphore_wait() call.
 * Expect:  returns immediately (1), and sem1.count drops to 0.
 *
 * This exercises the "Case A" branch of semaphore_wait(): a token
 * was already available, so we took it without ever touching the
 * wait list.
 *===========================================================================*/
static Semaphore_t sem1;

static void test1_task(void)
{
    uint8_t ok = semaphore_wait(&sem1);

    usart2_send_string("Test 1 (token available): ");
    usart2_send_string((ok == 1 && sem1.count == 0) ? "PASS\r\n" : "FAIL\r\n");

    /* Park forever -- see design note 1. */
    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Test 2 & 3 - Blocking, then a signal wakes the waiter.
 *
 * Setup:   sem23 initialized with count = 0.
 * Action:  waiter (P3) blocks in semaphore_wait().
 *          signaler (P4) runs a moment later, calls semaphore_signal().
 * Expect:  waiter wakes, semaphore_wait() returns 1.
 *
 * This is the "Case B" branch of semaphore_wait(): no token was
 * available, so the task marked itself BLOCKED, joined the wait
 * list, and yielded. When the signaler runs and calls
 * semaphore_signal(), the direct-handoff path in semaphore.c picks
 * the waiter off the wait list and marks it READY -- the waiter
 * never re-checks count; it just resumes as if it had gotten the
 * token all along.
 *
 * Priorities: waiter P3, signaler P4. See design note 2.
 *===========================================================================*/
static Semaphore_t sem23;

static void test2_waiter_task(void)
{
    usart2_send_string("Test 2 (blocking): waiter calling wait...\r\n");

    /* Blocks here until signaled. */
    uint8_t ok = semaphore_wait(&sem23);

    usart2_send_string("Test 3 (direct handoff): waiter woke up: ");
    usart2_send_string((ok == 1) ? "PASS\r\n" : "FAIL\r\n");

    for (;;) { semaphore_wait(&testDoneSem); }
}

static void test2_signaler_task(void)
{
    /* Give the waiter time to actually reach semaphore_wait() and
     * block. See design note 2 for why this order matters. */
    delay_ticks(50);

    usart2_send_string("Test 2/3: signaling...\r\n");
    semaphore_signal(&sem23);

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Test 4 - Signal with no waiter increments count.
 *
 * Setup:   sem4 initialized with count = 0, no tasks waiting.
 * Action:  call semaphore_signal(&sem4).
 * Expect:  sem4.count becomes 1 (the token is deposited for a
 *          future waiter), and signal returns 1.
 *
 * This exercises the "else" branch of semaphore_signal(): nobody
 * was on the wait list, so the signal just increments the count.
 *===========================================================================*/
static Semaphore_t sem4;

static void test4_task(void)
{
    uint8_t ok = semaphore_signal(&sem4);

    usart2_send_string("Test 4 (signal, no waiter): ");
    usart2_send_string((ok == 1 && sem4.count == 1) ? "PASS\r\n" : "FAIL\r\n");

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Test 5 - Multiple tokens and exhaustion.
 *
 * Setup:   sem5 initialized with count = 3.
 * Action:  four sequential semaphore_wait() calls.
 * Expect:  the first three return immediately (count 3 -> 2 -> 1 -> 0).
 *          The fourth blocks forever (nothing will signal sem5).
 *
 * "Blocks forever" is the assertion here -- if the fourth wait ever
 * returns, that's a bug in the count logic. The absence of further
 * output after "Test 5b" is the PASS condition.
 *===========================================================================*/
static Semaphore_t sem5;

static void test5_task(void)
{
    /* These three succeed immediately; count drops 3 -> 2 -> 1 -> 0. */
    uint8_t r1 = semaphore_wait(&sem5);
    uint8_t r2 = semaphore_wait(&sem5);
    uint8_t r3 = semaphore_wait(&sem5);

    uint8_t firstThreeOk = (r1 == 1 && r2 == 1 && r3 == 1 && sem5.count == 0);

    usart2_send_string("Test 5 (three tokens then exhausted): ");
    usart2_send_string(firstThreeOk ? "PASS\r\n" : "FAIL\r\n");

    usart2_send_string("Test 5b: 4th wait now blocks (no further output expected)...\r\n");

    /* Deliberately blocks forever -- nothing ever signals sem5. */
    semaphore_wait(&sem5);

    /* Unreachable in a correct kernel. If we get here, print FAIL. */
    usart2_send_string("Test 5b: FAIL - 4th wait returned without a signal\r\n");

    for (;;) { semaphore_wait(&testDoneSem); }
}

/*===========================================================================
 * Entry point called from main.c's main().
 *===========================================================================*/
void semaphore_tests_basic_create_tasks(void)
{
    usart2_send_string("=== Chapter 29 Semaphore Tests: Basic (1-5) ===\r\n");

    /* Must be initialized before any task can block on it. */
    semaphore_init(&testDoneSem, 0); /* never signaled */

    /* Test 1: one initial token, no blocking. */
    semaphore_init(&sem1, 1);
    kernel_create_task(test1_task, "T1", 5);

    /* Test 2 & 3: waiter (P3) blocks, signaler (P4) wakes it.
     * P4 < P3 is required -- see design note 2. */
    semaphore_init(&sem23, 0);
    kernel_create_task(test2_waiter_task,   "T2W", 3);
    kernel_create_task(test2_signaler_task, "T2S", 4);

    /* Test 4: signal with nobody waiting. */
    semaphore_init(&sem4, 0);
    kernel_create_task(test4_task, "T4", 5);

    /* Test 5: multiple tokens then exhaustion. */
    semaphore_init(&sem5, 3);
    kernel_create_task(test5_task, "T5", 5);

    /* 5 tasks total -- well within MAX_TASKS = 8. */
}

#endif /* APP_MODE_TEST_BASIC */
