# Chapter 28 — Blocking, Delays, Idle Task & Mutexes

## Overview

Chapter 28 introduces **blocking and synchronization**.

The RTOS can now move tasks out of the READY state when they need to wait,
wake them when the required condition is satisfied, and protect shared
resources using mutexes.

---

## Key Concepts

- Task delays / sleeping
- System tick
- Sleep list
- Idle task
- Blocking and wake-up
- Mutex
- Mutex ownership
- Mutex wait list
- Direct mutex handoff
- Critical sections
- Priority inversion
- Deadlock

---

## Task Delay

A task can voluntarily sleep for a specified number of ticks:

```
taskDelay(N)
     ↓
wakeTick = currentTick + N
     ↓
TASK_BLOCKED
     ↓
Sleep List
     ↓
Tick reaches wakeTick
     ↓
TASK_READY
```

The task does not consume CPU time while sleeping.

The RTOS uses an **absolute wake-up tick** rather than repeatedly
decrementing a delay counter.

---

## Sleep List

Sleeping tasks are kept separately from the five priority-based ready lists.

```
Ready Lists
├── P1
├── P2
├── P3
├── P4
└── P5

Sleep List
└── Tasks waiting for their wake-up tick
```

When SysTick detects that a task's wake-up time has arrived, the task is
returned to the ready list corresponding to its priority.

---

## `taskDelay(0)`

`taskDelay(0)` does nothing.

Voluntary CPU yielding is kept conceptually separate from delaying/sleeping.

---

## Idle Task

If no normal task is READY, the RTOS runs an **idle task**.

```
Normal READY task?
       ↓
      YES → Run it
       ↓
      NO
       ↓
   Idle Task
       ↓
    __WFI()
```

The idle task prevents the scheduler from having no task to execute and can
place the CPU into a low-power wait state.

---

## Blocking

Blocking means:

> The task cannot currently make progress, so it is removed from the READY
> queue until the condition it is waiting for becomes true.

A blocked task remains alive in its TCB.

```
READY
  ↓
Condition unavailable
  ↓
BLOCKED
  ↓
Condition satisfied
  ↓
READY
```

---

## Mutex

A mutex provides **mutual exclusion** for a shared resource.

The mutex contains:

```c
typedef struct {
    bool locked;
    TCB_t *owner;
    List_t waitList;
} Mutex_t;
```

Conceptually:

```
Mutex
├── locked
├── owner
└── waiting tasks
```

---

## Mutex Lock

If the mutex is free:

```
mutex_lock()
     ↓
locked = true
owner = currentTask
     ↓
Continue
```

If the mutex is already owned:

```
mutex_lock()
     ↓
Mutex locked
     ↓
TASK_BLOCKED
     ↓
Mutex waitList
```

The waiting task does **not** spin or busy-wait.

---

## Mutex Ownership

Unlike a semaphore, a mutex has an owner.

Therefore:

```
Task A → locks mutex → Task A becomes owner
```

Only the owner is allowed to unlock it.

A mutex cannot be acquired from ISR context because an ISR cannot safely
block waiting for a mutex.

---

## Critical Sections

The check-and-acquire operation must be atomic.

The RTOS uses a short interrupt-masking critical section around the shared
mutex state:

```
Disable interrupts
       ↓
Check mutex state
       ↓
Acquire / modify state
       ↓
Enable interrupts
```

Interrupts are **not** disabled while a task waits for the mutex.

The critical section protects the small state transition, not the entire
waiting period.

---

## Direct Mutex Handoff

When the owner unlocks a mutex and tasks are waiting, ownership is transferred
directly to the highest-priority waiter.

```
Task A owns mutex
      ↓
Task A unlocks
      ↓
Select highest-priority waiter
      ↓
Transfer ownership
      ↓
Wake waiter
      ↓
Waiter continues as mutex owner
```

The mutex is not temporarily released and then reacquired.

This avoids another READY task stealing the mutex between unlock and wake-up.

---

## Priority-Aware Mutex Waiting

The mutex wait list is priority-aware:

```
P1 waiter
   ↓
P2 waiter
   ↓
P4 waiter
```

The highest-priority waiting task receives the mutex first.

For equal priorities, FIFO ordering is preserved.

---

## Priority Inversion

Priority inversion can occur when:

```
High-priority task
        ↓
Waiting for mutex
        ↓
Low-priority task owns mutex
        ↓
Medium-priority task runs
```

The high-priority task is indirectly delayed by the lower-priority task.

Priority inheritance was studied as a solution, but it is **not implemented**
in this chapter.

---

## Deadlock

Multiple mutexes can create circular waiting:

```
Task A → owns Mutex 1 → waits for Mutex 2
Task B → owns Mutex 2 → waits for Mutex 1
```

Neither task can continue.

A simple prevention strategy is to use a **consistent mutex acquisition
order**.

No deadlock detector is implemented.

---

## Scheduler Interaction

Blocking and wake-up integrate with the existing priority scheduler:

```
Task blocks / wakes
       ↓
Ready-list membership changes
       ↓
Scheduler evaluates priorities
       ↓
PendSV performs context switch
```

The scheduler still decides **who should run**, while PendSV handles the
actual CPU context switch.

---

## Result

Chapter 28 extends the RTOS from merely scheduling tasks to allowing tasks to
**wait efficiently and safely share resources**.

```
Task
 ↓
Block / Sleep
 ↓
Wake
 ↓
Ready Queue
 ↓
Priority Scheduler
 ↓
Mutex Synchronization
```

---

## Status

**Completed**

---

### Main Takeaway

> Blocking removes a task from CPU competition until it can make progress;
> a mutex protects shared resources through ownership; and direct handoff
> ensures the mutex passes safely to the appropriate waiting task.
