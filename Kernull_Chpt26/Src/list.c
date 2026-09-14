/******************************************************************************
 * File        : list.c
 * Project     : Educational RTOS
 * Chapter     : 26 - Linked List Based Task Queues
 *
 * Description:
 *  Implements the generic singly-linked list declared in list.h.
 *
 *  Every function here only ever touches head, tail, and 'next'
 *  pointers -- there is deliberately no reference anywhere to
 *  TaskState, READY, BLOCKED, or the scheduler. That separation is
 *  the entire point of pulling this into its own module: kernel.c
 *  decides what a list MEANS, list.c only implements how a list
 *  WORKS.
 ******************************************************************************/

#include "list.h"

/*===========================================================================
 *                              list_init
 *===========================================================================*/

/*
 * An empty list is simply one where both ends point at nothing.
 */
void list_init(List_t *list)
{
    list->head = 0;
    list->tail = 0;
}

/*===========================================================================
 *                              list_append
 *===========================================================================*/

/*
 * Adds 'task' as the new last node in the list.
 *
 * Two cases:
 *
 *  1. The list is currently empty.
 *     head and tail both become 'task' -- it is simultaneously the
 *     first and last (and only) node.
 *
 *  2. The list already has at least one node.
 *     The current tail's 'next' is pointed at 'task', and then
 *     'task' becomes the new tail. The old tail no longer needs to
 *     be touched again, which is exactly why keeping a tail pointer
 *     around (instead of walking the list to find the end) is what
 *     makes this an O(1) operation instead of O(n).
 */
void list_append(List_t *list, TCB_t *task)
{
    /*
     * 'task' is about to become the last node, so nothing follows it
     * yet. Setting this explicitly (rather than trusting it was left
     * clean by whoever last used this TCB) is what makes list_append()
     * safe to call on a task that was just popped from a different
     * list a moment ago.
     */
    task->next = 0;

    if (list->head == 0)
    {
        /* List was empty -- this task is now the whole list. */
        list->head = task;
        list->tail = task;
    }
    else
    {
        /* List already has nodes -- attach after the current tail. */
        list->tail->next = task;
        list->tail        = task;
    }
}

/*===========================================================================
 *                              list_pop_front
 *===========================================================================*/

/*
 * Removes and returns the head node.
 *
 * The head's successor becomes the new head. If that successor is
 * NULL, the list has just become empty, and the tail pointer must be
 * cleared too -- otherwise it would keep pointing at a node that is
 * no longer part of this list, and a later list_append() would wrongly
 * try to link a new node onto it.
 */
TCB_t *list_pop_front(List_t *list)
{
    TCB_t *task = list->head;

    if (task == 0)
    {
        /* Nothing to pop. Caller's responsibility to expect this. */
        return 0;
    }

    list->head = task->next;

    if (list->head == 0)
    {
        /* That was the last node -- the list is now empty. */
        list->tail = 0;
    }

    /*
     * Fully detach the returned node from this list. Without this,
     * 'task' would still carry a 'next' pointer into a list it no
     * longer belongs to, which could corrupt whichever list it gets
     * appended to next.
     */
    task->next = 0;

    return task;
}

/*===========================================================================
 *                              list_remove
 *===========================================================================*/

/*
 * Removes 'task' from the list, wherever it happens to be.
 *
 * Because this is a singly-linked list, removing a node requires
 * finding the node BEFORE it (there is no 'prev' pointer to walk
 * backwards from). Three cases:
 *
 *  1. 'task' is the head.
 *     No predecessor to update -- just move head forward.
 *
 *  2. 'task' is in the middle or is the tail.
 *     Walk forward from head until the node whose 'next' IS 'task'
 *     is found, then splice 'task' out by pointing that node's
 *     'next' past it.
 *
 *  3. 'task' isn't in this list at all.
 *     The search reaches the end without finding it, and the list is
 *     left completely unchanged.
 *
 * In every case where 'task' was the tail, list->tail must be
 * updated too, or it would be left pointing at a node that is no
 * longer part of the list.
 */
void list_remove(List_t *list, TCB_t *task)
{
    if (list->head == 0 || task == 0)
    {
        /* Empty list, or nothing was asked to be removed. */
        return;
    }

    if (list->head == task)
    {
        list->head = task->next;

        if (list->tail == task)
        {
            /* 'task' was the only node in the list. */
            list->tail = 0;
        }

        task->next = 0;
        return;
    }

    /*
     * Walk forward looking for the node whose 'next' points at
     * 'task'. Stops either when that node is found, or when the end
     * of the list is reached ('task' was never in this list).
     */
    TCB_t *prev = list->head;

    while (prev->next != 0 && prev->next != task)
    {
        prev = prev->next;
    }

    if (prev->next == task)
    {
        prev->next = task->next;

        if (list->tail == task)
        {
            /* 'task' was the tail -- the node before it is now the tail. */
            list->tail = prev;
        }

        task->next = 0;
    }

    /*
     * else: reached the end of the list without finding 'task'.
     * Nothing to do -- the list did not contain this node.
     */
}

/*===========================================================================
 *                          End of File
 *===========================================================================*/
