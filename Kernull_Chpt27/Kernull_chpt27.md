# Chapter 27 — Priority-Based Scheduling

## Overview

Chapter 27 extends the Round-Robin scheduler by introducing **fixed-priority
preemptive scheduling**.

The RTOS now selects tasks primarily by priority and uses Round Robin only
between tasks having the same priority.

---

## Key Concepts

- Fixed task priorities
- Five priority levels
- Priority-based preemption
- Multiple ready queues
- Round Robin within equal priority
- Priority-aware task wake-up
- Scheduler policy

---

## Priority Levels

The RTOS uses exactly five priority levels:

```
P1 → Highest
P2
P3
P4
P5 → Lowest
```

A task's priority is assigned when the task is created and remains fixed.

```c
kernel_create_task(taskFunc, name, priority);
```

Valid priorities are **1–5**.

---

## Multiple Ready Lists

Instead of maintaining one ready list, the kernel maintains one list for
each priority:

```
readyList[0] → P1 tasks
readyList[1] → P2 tasks
readyList[2] → P3 tasks
readyList[3] → P4 tasks
readyList[4] → P5 tasks
```

The mapping is:

```
priority 1 → readyList[0]
priority 2 → readyList[1]
...
priority 5 → readyList[4]
```

An empty priority level simply has an empty corresponding list.

---

## Priority vs `TCB->next`

These have different purposes:

```
readyList[]
    ↓
selects the priority queue

TCB->next
    ↓
links tasks within that queue
```

For example:

```
P2 readyList
    ↓
Task A → Task C → Task F → NULL
```

---

## Scheduling Policy

The scheduler follows two rules:

### 1. Highest Priority Wins

The scheduler always selects the highest-priority READY task.

```
P1 READY?
   ↓
  YES → Run P1

  NO
   ↓
P2 READY?
   ↓
  YES → Run P2

  NO
   ↓
  ...
```

### 2. Round Robin Within the Same Priority

If multiple tasks have the same priority, they share CPU time using
Round Robin.

```
P2:
Task A → Task B → Task C
   ↑
 Round Robin
```

A higher-priority task does **not** rotate to a lower-priority task while it
remains READY.

---

## Preemption

If a higher-priority task becomes READY while a lower-priority task is
running, the higher-priority task preempts it.

```
P4 running
    ↓
P2 becomes READY
    ↓
PendSV requested
    ↓
P2 runs
```

When the higher-priority task later blocks, a lower-priority READY task can
run again.

---

## Scheduling Events

The scheduler may need to select another task when:

```
1. Time slice expires
2. Current task blocks
3. Higher-priority task becomes READY
```

These events ultimately lead to a PendSV-based context switch.

---

## Scheduler vs PendSV

The separation introduced earlier is maintained:

```
Scheduler
    ↓
Decides WHO should run

PendSV
    ↓
Handles HOW the CPU switches
```

The scheduler does not directly save or restore CPU registers.

---

## Why Multiple Ready Lists?

A single ready list would require repeatedly scanning tasks to determine
which READY task has the highest priority.

With five priority-specific lists:

```
readyList[0]
readyList[1]
readyList[2]
readyList[3]
readyList[4]
```

the scheduler only checks the five priority levels.

This keeps the implementation simple while making priority selection
straightforward.

---

## Task Control Block

The TCB now includes priority information:

```
TCB
├── Stack Pointer
├── Stack
├── Task Function
├── Task Name
├── Task State
├── Next
└── Priority
```

`currentTask` remains a pointer to the currently running TCB.

---

## Important Invariant

At all times:

```
Every READY task
    → belongs to exactly one appropriate ready list

Every BLOCKED task
    → belongs to no ready list

currentTask
    → identifies the task the CPU is intended to execute
```

Maintaining these invariants keeps scheduling and blocking operations
consistent.

---

## Result

Chapter 27 changes the scheduler from:

```
Round Robin only
```

to:

```
        Highest Priority
              ↓
       Priority Selection
              ↓
    ┌─────────┴─────────┐
    ↓                   ↓
Different Priority   Same Priority
    ↓                   ↓
   Preempt             Round Robin
```

This establishes the foundation for synchronization primitives introduced in
later chapters.

---

## Status

**Completed**

---

### Main Takeaway

> Priority determines **which group runs**; Round Robin determines **which
> task runs within that priority**; PendSV performs the actual context switch.
