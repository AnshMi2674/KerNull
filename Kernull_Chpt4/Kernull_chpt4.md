# Chapter 4 — Task & Process Abstraction

## Overview

Chapter 4 establishes the foundation of the RTOS by introducing the concept
of a **task** and representing each task using a **Task Control Block (TCB)**.

The goal is to move from simply executing functions to having the kernel
manage independent tasks.

---

## Key Concepts

- Task abstraction
- Task Control Block (TCB)
- Task state
- Task function
- Private task stack
- Static task allocation
- Basic task management

---

## Task Control Block

Each task is represented by a TCB containing the information required by the
kernel to manage that task.

Conceptually:

```
TCB
├── Stack information
├── Task function
├── Task name
└── Task state
```

The TCB becomes the kernel's representation of a task.

---

## Task States

The RTOS defines the basic task states:

```
READY
RUNNING
BLOCKED
```

At this stage, the focus is primarily on creating and managing tasks. The
`BLOCKED` state provides the foundation for later synchronization and waiting
mechanisms.

---

## Task Stacks

Each task is given its own private stack.

This is important because, once context switching is introduced, the RTOS
must be able to preserve and restore each task's execution state
independently.

```
Task A → Private Stack A
Task B → Private Stack B
Task C → Private Stack C
```

---

## Memory Design

Tasks and their stacks are statically allocated.

This keeps the educational RTOS simple and deterministic while avoiding
dynamic memory management.

---

## Foundation for Later Chapters

Chapter 4 establishes the fundamental relationship:

```
Task
  ↓
TCB
  ↓
Task State + Stack
  ↓
Scheduler
  ↓
Context Switching
```

The task abstraction introduced here becomes the foundation for the
scheduler, context switching, blocking, mutexes, and semaphores implemented
in later chapters.

---

## Status

**Completed**

---

## Main Takeaway

A task is more than a function. It is a function together with the state
and stack information required by the kernel to manage its execution.
