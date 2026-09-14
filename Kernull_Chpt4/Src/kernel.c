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

uint8_t kernel_create_task(void (*taskFunc)(void), const char *name)
{
    if (taskCount >= MAX_TASKS) {
        return 0;   // failure — process list is full
    }

    taskList[taskCount].taskFunc = taskFunc;
    taskList[taskCount].name     = name;
    taskList[taskCount].state    = TASK_READY;

    taskCount++;
    return 1;       // success
}

void kernel_start(void)
{
    if (taskCount == 0) {
        return;   // nothing to run — guards against % 0 below
    }

    while (1) {
        taskList[currentTask].state = TASK_RUNNING;
        taskList[currentTask].taskFunc();
        taskList[currentTask].state = TASK_READY;

        currentTask = (currentTask + 1) % taskCount;
    }
}
