# Chapter 29 — Semaphores

## Overview

Chapter 29 introduces **semaphores** as a synchronization mechanism for
resource counting and task/event synchronization.

Unlike a mutex, a semaphore does not have an owner. Its state is represented
primarily by a **count**.

---

## Key Concepts

- Semaphore
- Counting semaphore
- Binary semaphore
- Semaphore wait
- Semaphore signal
- Semaphore wait list
- Direct handoff
- Priority-aware waiting
- ISR-to-task synchronization
- Blocking and wake-up

---

## Semaphore Structure

The RTOS uses:

```c
typedef struct {
    uint32_t count;
    List_t waitList;
} Semaphore_t;
```

Conceptually:

```
Semaphore
├── Count
└── Waiting Tasks
```

The count represents the number of available tokens/resources.

---

## Semaphore Wait

`semaphore_wait()` attempts to consume one token.

If a token is available:

```
count > 0
   ↓
count--
   ↓
Task continues
```

If no token is available:

```
count == 0
   ↓
Task → BLOCKED
   ↓
Semaphore waitList
```

The task does not busy-wait or consume CPU time while waiting.

---

## Semaphore Signal

`semaphore_signal()` releases or produces one token.

If no task is waiting:

```
No waiter
   ↓
count++
```

If a task is waiting:

```
Waiter exists
     ↓
Select highest-priority waiter
     ↓
Wake task
     ↓
Directly satisfy its wait
     ↓
count remains unchanged
```

---

## Direct Handoff

The semaphore uses **direct handoff** when a waiter exists.

Instead of:

```
Signal → count++ → another task consumes token
```

the token is effectively transferred directly:

```
Signaler
   ↓
Waiting Task
   ↓
Task becomes READY
```

This prevents another READY task from stealing the resource before the
waiting task gets it.

---

## Priority-Aware Waiting

Semaphore waiters are selected according to the RTOS scheduling policy:

```
Highest priority waiter
        ↓
      Next
        ↓
      Next
```

For tasks with the same priority, FIFO ordering is maintained.

---

## Binary vs Counting Semaphore

### Binary Semaphore

```
count = 0 or 1
```

Useful for simple event signaling.

### Counting Semaphore

```
count = 0 ... N
```

Useful when multiple identical resources are available.

For example:

```
3 available buffers
      ↓
Semaphore count = 3
```

Each successful wait consumes one available resource.

---

## Semaphore vs Mutex

| Mutex | Semaphore |
|---|---|
| Has an owner | No owner |
| Protects shared resources | Counts resources/events |
| Owner unlocks | Any permitted context can signal |
| Cannot be acquired from ISR | Signal can be ISR-safe |
| Ownership matters | Token/count matters |

A semaphore can therefore be used for **task-to-task or ISR-to-task
synchronization**.

---

## ISR-to-Task Synchronization

A common RTOS pattern is:

```
Hardware Event
      ↓
ISR
      ↓
semaphore_signal()
      ↓
Wake waiting task
      ↓
Task performs heavier processing
```

The ISR handles the immediate hardware event, while the task performs the
larger processing outside interrupt context.

---

## Critical Sections

Semaphore state changes must be atomic.

The RTOS protects operations such as:

```
Check count
Remove waiter
Wake waiter
Increment count
```

using a short critical section.

The RTOS does **not** disable interrupts while a task is blocked.

---

## Scheduler Interaction

Semaphore operations integrate with the existing scheduler:

```
Task waits
   ↓
Removed from Ready List
   ↓
Added to Semaphore Wait List
   ↓
Another task / ISR signals
   ↓
Task becomes READY
   ↓
Priority scheduler evaluates
   ↓
PendSV performs context switch
```

The same scheduler/PendSV separation from earlier chapters is preserved.

---

## Important Invariant

At any time:

```
READY task
    → belongs to exactly one ready list

BLOCKED semaphore waiter
    → belongs to the semaphore wait list

Semaphore signal with waiter
    → wakes exactly one waiter

Semaphore signal without waiter
    → increments the count
```

Maintaining these invariants keeps semaphore synchronization consistent.

---

## Result

Chapter 29 completes the RTOS's basic synchronization mechanisms:

```
                Synchronization
                      │
             ┌────────┴────────┐
             ↓                 ↓
           Mutex           Semaphore
             │                 │
        Ownership        Tokens / Events
             │                 │
       Shared Resource    Resource Counting
                         ISR → Task Sync
```

The RTOS can now:

```
Schedule Tasks
      ↓
Block Tasks
      ↓
Wake Tasks
      ↓
Protect Shared Resources
      ↓
Synchronize Events
```

---

## Status

**Completed**

---

### Main Takeaway

> A mutex answers **"Who owns this resource?"**; a semaphore answers
> **"How many permissions/resources/events are available?"**
