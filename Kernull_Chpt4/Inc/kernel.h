#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════
//  Ch.4 — The process abstraction, minimal version
//
//  No real context switching yet (that's Ch.6/PendSV). Each "task"
//  here is just a function that does ONE STEP of work and returns.
//  The "kernel" is nothing more than: a list of these functions
//  (the process list), and a loop that calls each one in turn
//  (the scheduler — FIFO/round-robin, per Ch.7).
// ═══════════════════════════════════════════════════════════════

#define MAX_TASKS 8

typedef enum {
    TASK_READY,      // could run
    TASK_RUNNING     // currently executing (only meaningful WHILE its function is on the call stack)
} TaskState;

// This IS the PCB/TCB from Ch.4.5 — a plain C struct, nothing more.
typedef struct {
    void       (*taskFunc)(void);   // the task's entry point — one step of its logic
    const char  *name;               // debug label only, no behavioral effect (like pcName)
    TaskState    state;
} TCB_t;

void kernel_init(void);
uint8_t kernel_create_task(void (*taskFunc)(void), const char *name);
void kernel_start(void);   // never returns — this IS the process list dispatcher

#endif




/*
 *
 * void PendSV_Handler(void) {
    __asm volatile (
        "MRS R0, PSP                \n"  // R0 = current task's stack pointer
        "STMDB R0!, {R4-R11}        \n"  // manually push R4-R11 (hardware only auto-pushed R0-R3,R12,LR,PC,xPSR)
        "LDR R1, =currentTaskTCB    \n"  // find where to save it
        "LDR R2, [R1]               \n"
        "STR R0, [R2]               \n"  // save updated SP into the OUTGOING task's TCB

        "BL scheduler_pick_next     \n"  // your round-robin/priority logic decides who's next

        "LDR R1, =currentTaskTCB    \n"
        "LDR R2, [R1]               \n"
        "LDR R0, [R2]               \n"  // load the INCOMING task's saved SP
        "LDMIA R0!, {R4-R11}        \n"  // restore R4-R11 from its stack
        "MSR PSP, R0                \n"  // point PSP at the incoming task's stack
        "BX LR                      \n"  // exception return — hardware auto-pops R0-R3,R12,LR,PC,xPSR
    );
}
 *
 *
 */
