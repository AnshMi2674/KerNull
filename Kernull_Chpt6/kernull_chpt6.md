# Chapter 6 — Cortex-M3 Exceptions & First Task Launch

## Overview

Chapter 6 connects the RTOS task abstraction to the **ARM Cortex-M3
exception and stack architecture**.

The main objective is to understand how the processor handles exceptions and
how the RTOS uses this mechanism to launch the first task.

---

## Key Concepts

- Cortex-M3 exception model
- Thread Mode vs Handler Mode
- MSP vs PSP
- Hardware exception stacking
- Software-saved context
- Exception return
- SVC
- Artificial exception frame
- First task launch

---

## Thread Mode vs Handler Mode

The Cortex-M3 operates in two relevant modes:

```
Thread Mode  → Normal task/kernel execution
Handler Mode → Exception/interrupt execution
```

Exceptions automatically move the processor into Handler Mode.
After exception return, the processor can return to Thread Mode.

---

## MSP vs PSP

The Cortex-M3 provides two stack pointers:

```
MSP → Main Stack Pointer
PSP → Process Stack Pointer
```

The RTOS uses the stacks conceptually as:

```
Kernel / Exceptions → MSP
Tasks               → PSP
```

This allows task execution to use an independent process stack.

---

## Hardware Exception Stacking

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

This automatically created stack frame becomes the basis for the RTOS
context-switching mechanism developed in Chapter 7.

---

## Why SVC Is Required

The kernel cannot simply use a normal C function return to start the first
task. Instead, the RTOS uses `SVC` (Supervisor Call) to enter an exception
handler and then performs an exception return into the first task.

```
kernel_start()
      ↓
     SVC
      ↓
 SVC Handler
      ↓
Prepare task context
      ↓
Exception Return
      ↓
Thread Mode + PSP
      ↓
First Task
```

---

## Artificial Exception Frame

The RTOS creates a fake exception frame that looks like a frame produced by
the processor during a real exception.

```
Higher Address
┌────────────┐
│   xPSR     │
│    PC      │
│    LR      │
│    R12     │
│    R3      │
│    R2      │
│    R1      │
│    R0      │
└────────────┘
Lower Address
```

The task's:

- `PC` points to the task function.
- `LR` points to the task-exit/error handler.
- `xPSR` contains the required Thumb-state bit.

The CPU can then restore this artificial frame using its normal
exception-return mechanism.

---

## First Task Launch

The overall mechanism is:

```
Fake Task Context
       ↓
SVC Handler
       ↓
Set PSP
       ↓
Exception Return
       ↓
Thread Mode
       ↓
Task Function
```

The RTOS therefore does not need a special hardware instruction such as
"start task". It prepares the CPU's expected stack frame and uses the
existing Cortex-M exception mechanism.

---

## Important Insight

The exception mechanism is not only used for interrupts. The RTOS
deliberately uses it as a controlled mechanism for entering task execution
and later for performing context switches.

---

## Status

**Completed**

---

## Main Takeaway

The RTOS uses the Cortex-M3 exception and stack mechanism to make the CPU
enter a task as if it were returning from an exception.
