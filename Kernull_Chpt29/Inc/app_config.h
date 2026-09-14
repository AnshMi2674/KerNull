/******************************************************************************
 * File        : app_config.h
 * Project     : Educational RTOS
 * Chapter     : 29 - Semaphores
 *
 * Description:
 *  STM32CubeIDE (and most simple embedded build setups) compiles and
 *  links EVERY .c file sitting in Src/ into one binary - there is no
 *  "build this file instead of that one" the way a hosted project
 *  might have separate executable targets. That means main.c,
 *  semaphore_tests_basic.c, and semaphore_tests_priority.c cannot
 *  each supply their own main() / SysTick_Handler() / get_tick() -
 *  the linker will (correctly) reject that as multiple definitions,
 *  exactly as it just did.
 *
 *  The fix: exactly ONE main(), SysTick_Handler(), and get_tick()
 *  exist in the whole project - all three stay in main.c, unchanged
 *  in shape from Chapter 28. What VARIES is which set of tasks
 *  main() creates, selected by this single compile-time constant.
 *
 *  To switch what the board runs, change ONE line below and rebuild -
 *  nothing else in the project needs touching.
 ******************************************************************************/

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_MODE_APPLICATION    0   /* real LED/Temp/Comms app (Ch. 28/29) */
#define APP_MODE_TEST_BASIC     1   /* semaphore Tests 1-5   */
#define APP_MODE_TEST_PRIORITY  2   /* semaphore Tests 6-8   */

/*
 * <<< CHANGE THIS LINE TO SELECT WHAT BUILDS >>>
 */
#define ACTIVE_APP_MODE  APP_MODE_APPLICATION

#endif /* APP_CONFIG_H */
