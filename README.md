# Educational RTOS — ARM Cortex-M3

A preemptive Real-Time Operating System built from scratch for the
**STM32F103RBT6 (ARM Cortex-M3)**.

This project was built to understand what actually happens inside an RTOS —
from task management and scheduling to CPU context switching and
synchronization — rather than simply using an existing RTOS such as FreeRTOS.

The project was developed progressively across **Chapters 4–29**, with each
chapter introducing and validating a new RTOS concept.

---

## What I Built

The RTOS progressively implements:

- Task Control Blocks (TCBs) and task management
- Task states and private task stacks
- Round-robin scheduling
- Preemptive scheduling
- Fixed-priority scheduling with 5 priority levels
- Linked-list based ready and blocked queues
- ARM Cortex-M3 exception handling
- MSP / PSP stack management
- SVC-based first task launch
- PendSV-based context switching
- Artificial exception return
- Task blocking and waking
- Mutexes with priority-aware waiting
- Binary and counting semaphores
- ISR-to-task synchronization
- Critical sections and atomic operations

The final design uses:

```
                    ┌─────────────────┐
                    │      Tasks      │
                    │   TCB + Stack   │
                    └────────┬────────┘
                             │
                    READY / BLOCKED
                             │
                    ┌────────▼────────┐
                    │    Scheduler    │
                    │ Priority + RR   │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │     PendSV      │
                    │ Save / Restore  │
                    │     Context     │
                    └────────┬────────┘
                             │
                         Next Task
```

---

## Scheduling Policy

The scheduler uses 5 fixed priority levels:

```
P1 > P2 > P3 > P4 > P5
```

- The highest-priority READY task always runs.
- Tasks at the same priority are scheduled using Round Robin.
- A higher-priority task becoming READY can preempt a lower-priority task.

---

## Target Hardware

| Component | Detail |
|---|---|
| MCU | STM32F103RBT6 |
| Core | ARM Cortex-M3 |
| Clock | 72 MHz |
| Debugger | ST-Link / SWD |

The RTOS was tested and debugged on real STM32 hardware rather than only
being simulated or compiled on a host machine.

---

## Debugging & Validation

A major part of the project was debugging the RTOS at the CPU level.

I used:

- STM32CubeIDE
- ARM GCC (`arm-none-eabi-gcc`)
- GDB
- ST-Link / SWD
- Expressions and Watch windows
- Memory inspection
- Cortex-M fault status registers
- Exception-frame inspection
- Vector-table inspection
- Disassembly
- Instruction-level stepping

Several bugs involved the Cortex-M exception mechanism, stack frames,
context restoration, interrupt timing, mutex handoff, semaphore scheduling,
and task starvation.

This debugging process was an important part of understanding how the RTOS
actually interacts with the processor.

See [`DEBUGGING_JOURNAL.md`](./DEBUGGING_JOURNAL.md) for the complete debugging history.

---

## Resource Usage

Final firmware footprint on the STM32F103RBT6:

| Resource | Usage |
|---|---|
| RAM | ~29.86% |
| Flash | ~4.22% |

This includes the RTOS and the application used for hardware validation.

---

## Learning Resources

The project was developed by combining operating-system theory with
Cortex-M architecture knowledge.

**Operating Systems**

*Operating Systems: Three Easy Pieces* (OSTEP)

Used primarily for understanding:
- Scheduling
- Concurrency
- Processes / tasks
- Locks
- Semaphores
- Synchronization
- Blocking

**ARM Cortex-M**

Joseph Yiu — *The Definitive Guide to the ARM Cortex-M3*

Used to understand:
- Cortex-M3 architecture
- Exceptions
- MSP and PSP
- Exception stacking
- SVC
- PendSV
- Exception return
- Context switching

The goal was to connect the OS-level concepts from OSTEP with their
actual implementation on a Cortex-M processor.

---

## Project Structure

The repository is organized progressively by chapter. Each chapter
represents a stage in the development of the RTOS and preserves the
implementation and design decisions made at that point.

Detailed chapter documentation will be added separately.

```
Educational-RTOS/
│
├── README.md                    ← this file: project overview
│
├── DEBUGGING_JOURNAL.md         ← real debugging/war stories
│
├── Chapter-04/
│   └── README.md
│
├── Chapter-05/
│   └── README.md
│
│       ...
│
├── Chapter-28/
│   └── README.md
│
└── Chapter-29/
    └── README.md
```

---

## Why I Built This

The goal of this project was not to create another production-ready RTOS.

The goal was to understand the system from the inside:

**OS theory → CPU architecture → RTOS implementation → real hardware → low-level debugging**

Instead of only learning what an RTOS does, this project was an attempt to
understand how the processor actually makes it happen.

---

## Status

**Completed — Chapters 4–29**

The current implementation is an educational RTOS designed for learning,
experimentation, and understanding Cortex-M real-time systems.

---

## Author

**Ansh Mishra**
2nd Year EXTC Engineering Student
Interested in Embedded Systems & Firmware Development
