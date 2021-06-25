#include "enclave/threading.h"
#include <stdbool.h>
#include <stddef.h>
#include "enclave/synch.h"

size_t total_num_threads;

static spinlock_t thread_work_lock;
static struct thread_work *volatile work_head;
static struct thread_work *volatile work_tail;
static volatile bool work_done;

void thread_work_push(struct thread_work *work) {
    spinlock_lock(&thread_work_lock);
    work->next = NULL;
    if (!work_tail) {
        /* Empty list. Set head and tail. */
        work_head = work;
        work_tail = work;
    } else {
        /* List has values. */
        work_tail->next = work;
        work_tail = work;
    }
    spinlock_unlock(&thread_work_lock);
}

struct thread_work *thread_work_pop(void) {
    struct thread_work *work = NULL;
    while (!work) {
        while (!work_head) {
            if (work_done) {
                goto exit;
            }
        }
        spinlock_lock(&thread_work_lock);
        if (work_head) {
            work = work_head;
            if (!work_head->next) {
                work_tail = NULL;
            }
            work_head = work_head->next;
        }
        spinlock_unlock(&thread_work_lock);
    }
exit:
    return work;
}

void thread_start_work(void) {
    /* Wait for all threads to start work. */
    thread_wait_for_all();

    struct thread_work *work = thread_work_pop();
    while (work) {
        work->func(work->arr, work->start, work->length, work->descending,
                work->num_threads);
        sema_up(&work->done);
        work = thread_work_pop();
    }

    /* Wait for all threads to exit work loop. */
    thread_wait_for_all();
}

void thread_wait_for_all(void) {
    static size_t num_threads_waiting;
    static condvar_t all_threads_finished;
    static spinlock_t all_threads_lock;

    spinlock_lock(&all_threads_lock);
    num_threads_waiting++;
    if (num_threads_waiting >= total_num_threads) {
        condvar_broadcast(&all_threads_finished, &all_threads_lock);
        num_threads_waiting = 0;
    } else {
        condvar_wait(&all_threads_finished, &all_threads_lock);
    }
    spinlock_unlock(&all_threads_lock);
}

void thread_release_all(void) {
    work_done = true;
}

void thread_unrelease_all(void) {
    work_done = false;
}