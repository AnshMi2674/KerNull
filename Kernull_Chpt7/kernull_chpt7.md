# Chapter 7 — Preemptive Context Switching with PendSV

## Overview

Chapter 7 implements the first complete **preemptive context-switching
mechanism** of the RTOS.

The Cortex-M3 **SysTick** and **PendSV** exceptions are used together to
periodically trigger scheduling and switch between tasks.

---

## Key Concepts

- SysTick
- PendSV
- Preemptive scheduling
- CPU context saving and restoring
- PSP-based task stacks
- R4–R11 software context
- Scheduler
- SVC first-task launch
- Exception return

---

## Hardware vs Software Context Saving

When an exception occurs, the Cortex-M3 automatically saves:

```
R0
R1
R2
R3
R12
LR
PC
xPSR
```

The RTOS software additionally saves:

```
R4-R11
```

Together, these preserve the task's CPU context.

---

## Context Switch Flow

```
              SysTick
                 ↓
        Request PendSV
                 ↓
          PendSV Handler
                 ↓
          Save R4-R11
                 ↓
            Save PSP
                 ↓
       Scheduler selects task
                 ↓
          Load new PSP
                 ↓
        Restore R4-R11
                 ↓
        Exception Return
                 ↓
            Next Task
```

---

## Why PendSV?

PendSV is used for deferred context switching.

It is configured at the lowest exception priority, allowing higher-priority
interrupt handlers to complete before the RTOS performs the context switch.

This keeps context switching separate from normal peripheral interrupt
handling.

---

## SVC vs PendSV vs SysTick

Each exception has a separate responsibility:

| Exception | Responsibility |
|---|---|
| SVC | Launch the first task |
| SysTick | Generate periodic RTOS ticks |
| PendSV | Perform context switches |

This separation keeps the RTOS architecture clean.

---

## Scheduler vs Context Switch

The scheduler decides which task should run.

PendSV handles how the CPU switches to that task.

```
Scheduler
    │
    │ "Task B should run"
    ↓
PendSV
    │
    │ "Restore Task B's context"
    ↓
Task B
```

The TCB stores the task's saved stack pointer so its execution can later be
resumed.

---

## Fake Task Context

Every newly created task receives an artificial initial exception frame.

When that task is selected for the first time, the RTOS restores this
context and the processor enters the task through the normal exception-return
mechanism.

```
Fake Context
     ↓
Exception Return
     ↓
CPU restores context
     ↓
Task begins execution
```

---

## Exception Return

The RTOS uses:

```
0xFFFFFFFD
```

as the exception-return value to return to:

```
Thread Mode + PSP
```

This allows the task to execute using its own process stack.

---

## Result

Chapter 7 establishes the core mechanism that makes the RTOS preemptive:

```
Multiple Tasks
      ↓
Periodic SysTick
      ↓
Scheduler
      ↓
PendSV
      ↓
Save / Restore Context
      ↓
Another Task Executes
```

The RTOS can now interrupt a task, preserve its CPU state, select another
task, and later resume the previous task from where it stopped.

---

## Status

**Completed**

---

## Main Takeaway

The scheduler decides who runs; PendSV performs the context switch;
and the TCB stores where the task can be resumed.
