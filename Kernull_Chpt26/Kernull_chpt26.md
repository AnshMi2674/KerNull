# Chapter 26 — Linked Lists & Ready Queues

## Overview

Chapter 26 replaces the earlier array-based task scanning approach with
**linked lists**.

The goal is to make task management cleaner and prepare the RTOS for
multiple ready queues, blocking, and synchronization mechanisms.

## Key Concepts

- Linked lists
- Ready list
- Blocked list
- TCB linked-list nodes
- `head` and `tail`
- O(1) task insertion
- TCB-owned task storage
- Scheduler/list separation

## List Structure

The RTOS uses a generic list structure:

```c
typedef struct {
    TCB_t *head;
    TCB_t *tail;
} List_t;

Each TCB contains a next pointer:

TCB A → TCB B → TCB C → NULL

head points to the first task and tail points to the last task.

Ready List

The kernel maintains a list of tasks that are currently ready to execute:

readyList

head
 ↓
Task A → Task B → Task C → NULL
                       ↑
                      tail

When a task is created in the READY state, it is immediately added to the
appropriate ready list.

Blocked List

Tasks that cannot currently execute are removed from the ready list and placed
into a blocked list.

READY                     BLOCKED

Task A → Task C           Task B → Task D

A blocked task remains represented by its TCB; only its scheduling state and
list membership change.

Why Linked Lists?

The earlier approach required scanning the task array to find tasks.

With linked lists:

Task creation
     ↓
Append to list
     ↓
Scheduler operates on ready tasks

Using both head and tail allows appending a task to the end of a list in
O(1) time.

This is especially useful for Round-Robin scheduling because a task can be
moved to the back of its ready queue without scanning the entire task array.

Ownership vs Linking

The task array still owns the actual TCB memory:

taskList[]
 ├── TCB A
 ├── TCB B
 └── TCB C

The linked lists only contain pointers to those TCBs.

taskList[]  → owns TCB memory
readyList   → references READY TCBs
blockedList → references BLOCKED TCBs

This keeps the RTOS statically allocated while allowing flexible task
organization.

Current Task

currentTask is changed from an array index to a TCB pointer:

TCB_t *currentTask;

This allows the kernel to directly access the currently running task's TCB.

Scheduler Separation

The scheduler decides which task should run.

The lists provide the scheduler with the organization of tasks.

PendSV still handles the actual CPU context save/restore.

Lists
  ↓
Scheduler
  ↓
Select task
  ↓
PendSV
  ↓
Context switch

The scheduler does not need to manipulate CPU registers directly.

Foundation for Priority Scheduling

The linked-list architecture prepares the RTOS for the next scheduling
improvement:

Multiple Ready Queues
        ↓
Priority Scheduling
        ↓
Blocking / Wake-up
        ↓
Mutexes & Semaphores

Later chapters can therefore move tasks between queues without changing the
underlying TCB storage model.

Important Invariant

At any time:

READY task
    → exists in exactly one ready list

BLOCKED task
    → exists in no ready list

currentTask
    → points to the task intended to execute

Maintaining these invariants prevents tasks from becoming lost or appearing
in multiple scheduling queues.

Status

Completed

Main Takeaway

The TCB owns the task; the linked lists organize tasks; the scheduler
decides which task runs; and PendSV performs the CPU context switch.
