/******************************************************************************
 * File        : list.h
 * Project     : Educational RTOS
 * Chapter     : 26 - Linked List Based Task Queues
 *
 * Description:
 *  A small, generic singly-linked list built directly on top of TCB_t.
 *
 *  Why a list module exists at all
 *  --------------------------------
 *  Chapter 7's scheduler found the next task by scanning taskList[]
 *  from the current index forward until it hit a READY task. That
 *  works, but it means "which tasks are eligible to run right now" is
 *  never represented as its own thing -- it's implicit in a linear
 *  scan and a state field. Once later chapters need to move tasks in
 *  and out of that eligibility set for reasons other than "just
 *  finished its timeslice" (blocking on a semaphore, waking up after
 *  a delay, etc.), scanning the whole array every time stops being
 *  good enough. A queue you can push to and pop from in O(1) is the
 *  natural fit.
 *
 *  Why this module is generic
 *  ---------------------------
 *  list.c has no idea what "READY" or "BLOCKED" mean. It only knows
 *  how to link TCB_t nodes together via their 'next' pointer. kernel.c
 *  is the only place that decides what a given List_t is FOR (readyList,
 *  blockedList, ...). This separation is what lets the same four
 *  functions serve every queue the kernel will ever need, in this
 *  chapter and in later ones.
 *
 *  Why singly-linked, and why no 'count' field
 *  ---------------------------------------------
 *  Every operation this kernel actually needs -- append to the back,
 *  pop from the front, remove an arbitrary node -- is achievable with
 *  just head/tail and a forward 'next' pointer. Adding a 'prev'
 *  pointer or a running count would only be useful for operations
 *  nothing in this kernel performs yet, so per the project's own
 *  design decisions, they're left out.
 ******************************************************************************/

#ifndef LIST_H
#define LIST_H

#include "kernel.h"   /* for TCB_t and its 'next' member */

/*===========================================================================
 *                              List Structure
 *===========================================================================*/

/*
 * A List_t does not own its nodes. It only points at TCB_t structs
 * that already live inside taskList[] (see kernel.c). This mirrors
 * design decision #1: taskList[] remains the single owner of every
 * TCB; lists merely describe an ordering over TCBs that already
 * exist. There is no malloc/free anywhere in this module.
 */
typedef struct
{
    TCB_t *head;   /* first task in the queue -- next to run/removed  */
    TCB_t *tail;   /* last task in the queue -- where new tasks join  */

} List_t;

/*===========================================================================
 *                              List API
 *===========================================================================*/

/*
 * Resets a list to the empty state.
 *
 * Must be called once on every List_t before it is used for
 * anything else -- an uninitialized head/tail pointing at garbage
 * would make every other operation below undefined behaviour.
 */
void list_init(List_t *list);

/*
 * Adds 'task' to the back of the list.
 *
 * Used both when a brand-new task is created (it joins the READY
 * queue immediately) and when a running task's timeslice ends but it
 * is still eligible to run (it rejoins the READY queue behind
 * whoever was already waiting) -- which is exactly what makes the
 * scheduler "round robin": tasks are served in the order they
 * arrived, and arrive again at the back every time they're re-queued.
 *
 * O(1) because the list keeps a tail pointer instead of walking to
 * the end on every call.
 */
void list_append(List_t *list, TCB_t *task);

/*
 * Removes and returns the task at the front of the list.
 *
 * Returns 0 (NULL) if the list is empty. The scheduler is expected
 * to only call this when it knows the list is non-empty (e.g. because
 * the task being switched out was just appended back into it), so an
 * empty READY queue at scheduling time indicates a genuine kernel bug
 * rather than a condition to silently paper over.
 */
TCB_t *list_pop_front(List_t *list);

/*
 * Removes 'task' from the list, wherever it currently sits --
 * head, tail, or somewhere in the middle.
 *
 * Not used by the Round Robin scheduler itself in this chapter (it
 * only ever pops from the front and appends to the back). This
 * exists now because it is the operation later chapters need to move
 * a task OUT of readyList the moment it blocks, rather than letting
 * it stay queued and get picked to run while it has nothing to do.
 * Implementing it as part of the generic list module now -- instead
 * of bolting it on later -- keeps all list-manipulation logic in one
 * place, per design decision #10.
 *
 * Safe to call with a task that isn't actually in the list; in that
 * case the list is left unchanged.
 */
void list_remove(List_t *list, TCB_t *task);

#endif /* LIST_H */
