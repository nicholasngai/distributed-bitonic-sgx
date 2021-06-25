#ifndef __DISTRIBUTED_SGX_SORT_ENCLAVE_THREADING_H
#define __DISTRIBUTED_SGX_SORT_ENCLAVE_THREADING_H

#include <stdbool.h>
#include <stddef.h>
#include "enclave/synch.h"

struct thread_work {
    void (*func)(void *arr, size_t start, size_t length, bool descending,
            size_t num_threads);
    void *arr;
    size_t start;
    size_t length;
    bool descending;
    size_t num_threads;

    sema_t done;

    struct thread_work *next;
};

extern size_t total_num_threads;

void thread_work_push(struct thread_work *work);
struct thread_work *thread_work_pop(void);
void thread_start_work(void);
void thread_wait_for_all(void);
void thread_release_all(void);
void thread_unrelease_all(void);

#endif /* distributed-sgx-sort/enclave/threading.h */