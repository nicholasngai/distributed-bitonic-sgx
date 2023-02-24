#include "enclave/nonoblivious.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/defs.h"
#include "common/elem_t.h"
#include "common/error.h"
#include "common/util.h"
#include "enclave/mpi_tls.h"
#include "enclave/parallel_enc.h"
#include "enclave/threading.h"

#define BUF_SIZE 1024
#define SAMPLE_PARTITION_BUF_SIZE 512

/* Compares elements by the tuple (key, ORP ID). The check for the ORP ID must
 * always be run (it must be oblivious whether the comparison result is based on
 * the key or on the ORP ID), since we leak info on duplicate keys otherwise. */
static int mergesort_comparator(const void *a_, const void *b_) {
    const elem_t *a = a_;
    const elem_t *b = b_;
    int comp_key = (a->key > b->key) - (a->key < b->key);
    int comp_orp_id = (a->orp_id > b->orp_id) - (a->orp_id < b->orp_id);
    return (comp_key << 1) + comp_orp_id;
}

/* Sort ARR[RUN_IDX * BUF_SIZE] to ARR[MIN((RUN_IDX + 1), LENGTH)]. The results
 * will be stored in the same * location as the inputs. */
struct mergesort_first_pass_args {
    elem_t *arr;
    size_t length;
};
static void mergesort_first_pass(void *args_, size_t run_idx) {
    struct mergesort_first_pass_args *args = args_;

    size_t run_start = run_idx * BUF_SIZE;
    size_t run_length = MIN(args->length - run_start, BUF_SIZE);

    /* Sort using libc quicksort. */
    qsort(args->arr + run_start, run_length, sizeof(*args->arr),
            mergesort_comparator);
}

/* Merge each BUF_SIZE runs of length RUN_LENGTH starting at element
 * ARR[RUN_IDX * RUN_LENGTH * BUF_SIZE] and writing them to a single run of
 * length RUN_LENGTH * BUF_SIZE at OUT[RUN_IDX * RUN_LENGTH * BUF_SIZE]. If
 * LENGTH - RUN_IDX * RUN_LENGTH * BUF_SIZE is less than RUN_LENGTH * BUF_SIZE,
 * then the number of runs is reduced, and the last run is truncated. */
struct mergesort_pass_args {
    elem_t *input;
    elem_t *output;
    size_t length;
    size_t run_length;
    int ret;
};
static void mergesort_pass(void *args_, size_t run_idx) {
    struct mergesort_pass_args *args = args_;
    int ret;

    /* Compute the current run length and start. */
    size_t run_start = run_idx * args->run_length * BUF_SIZE;
    size_t num_runs =
        MIN(CEIL_DIV(args->length - run_start, args->run_length), BUF_SIZE);

    /* Create index buffer. */
    size_t *merge_indices = malloc(num_runs * sizeof(*merge_indices));
    if (!merge_indices) {
        perror("Allocate merge index buffer");
        ret = errno;
        goto exit;
    }
    memset(merge_indices, '\0', num_runs * sizeof(*merge_indices));

    /* Merge the runs in the buffer and encrypt to the output array.
     * Nodes for which we have reach the end of the array are marked as
     * a dummy element, so we continue until all elems in buffer are
     * dummy elems. */
    size_t output_idx = 0;
    bool all_done;
    do {
        /* Scan for lowest elem. */
        // TODO Use a heap?
        size_t lowest_run;
        size_t lowest_idx;
        all_done = true;
        for (size_t j = 0; j < num_runs; j++) {
            size_t run_idx = j * args->run_length + merge_indices[j];

            if (merge_indices[j] >= args->run_length
                    || run_start + run_idx >= args->length) {
                continue;
            }
            if (all_done
                    || mergesort_comparator(&args->input[run_start + run_idx],
                        &args->input[run_start + lowest_idx]) < 0) {
                lowest_run = j;
                lowest_idx = run_idx;
            }
            all_done = false;
        }

        /* Break out of loop if all done. */
        if (all_done) {
            continue;
        }

        /* Copy lowest elem to output. */
        memcpy(&args->output[run_start + output_idx],
                &args->input[run_start + lowest_idx], sizeof(*args->input));
        merge_indices[lowest_run]++;
        output_idx++;
    } while (!all_done);

    ret = 0;

    free(merge_indices);
exit:
    if (ret) {
        __atomic_compare_exchange_n(&args->ret, &ret, 0, false,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
    ;
}

/* Non-oblivious sort. Based on an external mergesort algorithm. */
static int mergesort(elem_t *arr, elem_t *out, size_t length) {
    int ret;

    /* Start by sorting runs of BUF_SIZE. */
    struct mergesort_first_pass_args args = {
        .arr = arr,
        .length = length,
    };
    struct thread_work work = {
        .type = THREAD_WORK_ITER,
        .iter = {
            .func = mergesort_first_pass,
            .arg = &args,
            .count = CEIL_DIV(length, BUF_SIZE),
        },
    };
    thread_work_push(&work);
    thread_work_until_empty();
    thread_wait(&work);

    /* Merge runs of increasing length in a BUF_SIZE-way merge by reading the
     * next smallest element of run i into buffer[i], then merging and
     * encrypting to the output buffer. */
    elem_t *input = arr;
    elem_t *output = out;
    for (size_t run_length = BUF_SIZE; run_length < length;
            run_length *= BUF_SIZE) {
        /* BUF_SIZE-way merge. */
        struct mergesort_pass_args args = {
            .input = input,
            .output = output,
            .length = length,
            .run_length = run_length,
        };
        struct thread_work work = {
            .type = THREAD_WORK_ITER,
            .iter = {
                .func = mergesort_pass,
                .arg = &args,
                .count = CEIL_DIV(length, run_length * BUF_SIZE),
            },
        };
        thread_work_push(&work);
        thread_work_until_empty();
        thread_wait(&work);
        ret = args.ret;
        if (ret) {
            handle_error_string("Error in run-length %lu merges of mergesort",
                    run_length);
            goto exit;
        }

        /* Swap the input and output arrays. */
        elem_t *temp = input;
        input = output;
        output = temp;
    }

    /* If the final merging output (now the input since it would have been
     * swapped) isn't the output parameter, copy to the right place. */
    if (input != out) {
        memcpy(out, input, length * sizeof(*out));
    }

    ret = 0;

exit:
    return ret;
}

struct sample {
    uint64_t key;
    uint64_t orp_id;
};

static int elem_sample_comparator(const elem_t *a, const struct sample *b) {
    int comp_key = (a->key > b->key) - (a->key < b->key);
    int comp_orp_id = (a->orp_id > b->orp_id) - (a->orp_id < b->orp_id);
    return (comp_key << 1) + comp_orp_id;
}

static int distributed_quickselect_helper(elem_t *arr, size_t local_length,
        size_t local_start, size_t *targets, struct sample *samples,
        size_t *sample_idxs, size_t num_targets, size_t left, size_t right) {
    int ret;

    if (!num_targets) {
        ret = 0;
        goto exit;
    }

    /* Get the next master by choosing the lowest elem with a non-empty
     * slice. */
    bool ready = true;
    bool not_ready = false;
    int master_rank = -1;
    for (int i = 0; i < world_size; i++) {
        if (i != world_rank) {
            bool *flag = left < right ? &ready : &not_ready;
            ret =
                mpi_tls_send_bytes(flag, sizeof(*flag), i, QUICKSELECT_MPI_TAG);
            if (ret) {
                handle_error_string("Error sending ready flag to %d from %d",
                        i, world_rank);
                goto exit;
            }
        }
    }
    for (int i = 0; i < world_size; i++) {
        bool is_ready;
        if (i != world_rank) {
            ret =
                mpi_tls_recv_bytes(&is_ready, sizeof(is_ready), i,
                        QUICKSELECT_MPI_TAG, MPI_TLS_STATUS_IGNORE);
            if (ret) {
                handle_error_string(
                        "Error receiving ready flag from %d into %d", i,
                        world_rank);
                goto exit;
            }
        } else {
            is_ready = left < right;
        }

        if (is_ready && (master_rank == -1 || i < master_rank)) {
            master_rank = i;
        }
    }

    if (master_rank == -1) {
        handle_error_string("All ranks reported empty slice");
        ret = -1;
        goto exit;
    }

    /* Get pivot. */
    struct sample pivot;
    if (world_rank == master_rank) {
        /* Use first elem as pivot. This is a random selection since this
         * samplesort should happen after ORP. */
        pivot.key = arr[left].key;
        pivot.orp_id = arr[left].orp_id;

        /* Send pivot to all other elems. */
        for (int i = 0; i < world_size; i++) {
            if (i != world_rank) {
                ret =
                    mpi_tls_send_bytes(&pivot, sizeof(pivot), i,
                            QUICKSELECT_MPI_TAG);
                if (ret) {
                    handle_error_string("Error sending pivot to %d from %d", i,
                            world_rank);
                    goto exit;
                }
            }
        }
    } else {
        /* Receive pivot from master. */
        ret =
            mpi_tls_recv_bytes(&pivot, sizeof(pivot), master_rank,
                    QUICKSELECT_MPI_TAG, MPI_TLS_STATUS_IGNORE);
        if (ret) {
            handle_error_string("Error receiving pivot into %d from %d",
                    world_rank, master_rank);
            goto exit;
        }
    }

    /* Partition data based on pivot. */
    // TODO It's possible to do this in-place.
    size_t partition_left = left + (world_rank == master_rank);
    size_t partition_right = right;
    enum {
        PARTITION_SCAN_LEFT,
        PARTITION_SCAN_RIGHT,
    } partition_state = PARTITION_SCAN_LEFT;
    while (partition_left < partition_right) {
        switch (partition_state) {
        case PARTITION_SCAN_LEFT:
            /* Scan left for elements greater than the pivot. If found, start
             * scanning right. */
            if (elem_sample_comparator(&arr[partition_left], &pivot) > 0) {
                partition_state = PARTITION_SCAN_RIGHT;
            } else {
                partition_left++;
            }

            break;

        case PARTITION_SCAN_RIGHT:
            /* Scan right for elements less than the pivot. */

            /* If found, swap and start scanning left. */
            if (elem_sample_comparator(&arr[partition_right - 1], &pivot) < 0) {
                elem_t temp;
                memcpy(&temp, &arr[partition_right - 1], sizeof(temp));
                memcpy(&arr[partition_right - 1], &arr[partition_left],
                        sizeof(*arr));
                memcpy(&arr[partition_left], &temp, sizeof(*arr));

                partition_state = PARTITION_SCAN_LEFT;
                partition_left++;
                partition_right--;
            } else {
                partition_right--;
            }

            break;
        }
    }

    /* Finish partitioning by swapping the pivot into the center, if we are the
     * master. */
    if (world_rank == master_rank) {
        elem_t temp;
        memcpy(&temp, &arr[partition_right - 1], sizeof(temp));
        memcpy(&arr[partition_right - 1], &arr[left], sizeof(*arr));
        memcpy(&arr[left], &temp, sizeof(*arr));

        partition_right--;
    }

    /* Sum size of partitions across all ranks and get next updated split,
     * either cur_split or cur_split + SUM(partition_left). */
    size_t cur_pivot;
    if (world_rank == master_rank) {
        cur_pivot = partition_right;

        for (int i = 0; i < world_size; i++) {
            if (i != world_rank) {
                size_t remote_partition_right;
                ret =
                    mpi_tls_recv_bytes(&remote_partition_right,
                            sizeof(remote_partition_right), i,
                            QUICKSELECT_MPI_TAG, MPI_TLS_STATUS_IGNORE);
                if (ret) {
                    handle_error_string(
                            "Error receiving partition size from %d into %d",
                            i, world_rank);
                    goto exit;
                }
                cur_pivot += remote_partition_right;
            }
        }

        for (int i = 0; i < world_size; i++) {
            if (i != world_rank) {
                ret =
                    mpi_tls_send_bytes(&cur_pivot, sizeof(cur_pivot), i,
                            QUICKSELECT_MPI_TAG);
                if (ret) {
                    handle_error_string(
                            "Error sending current pivot to %d from %d", i,
                            world_rank);
                    goto exit;
                }
            }
        }
    } else {
        ret =
            mpi_tls_send_bytes(&partition_right, sizeof(partition_right),
                    master_rank, QUICKSELECT_MPI_TAG);
        if (ret) {
            handle_error_string(
                    "Error sending partition size from %d to %d",
                    world_rank, master_rank);
            goto exit;
        }

        ret =
            mpi_tls_recv_bytes(&cur_pivot, sizeof(cur_pivot), master_rank,
                    QUICKSELECT_MPI_TAG, MPI_TLS_STATUS_IGNORE);
        if (ret) {
            handle_error_string(
                    "Error receiving current pivot into %d from %d",
                    world_rank, master_rank);
            goto exit;
        }
    }

    /* Check which directions we need to iterate in, based on the current pivot
     * index. If there are smaller targets, then iterate on the left half. If
     * there are larger targets, then iterate on the right half. If there is a
     * matching target, then set the sample in the output. */
    size_t *geq_target =
        bsearch_ge(&cur_pivot, targets, num_targets, sizeof(*targets), comp_ul);
    size_t geq_target_idx = (size_t) (geq_target - targets);
    bool found_target = geq_target_idx < num_targets && *geq_target == cur_pivot;
    size_t gt_target_idx = geq_target_idx + found_target;

    /* If we found a target, set the sample and its index. */
    if (found_target) {
        size_t i = geq_target - targets;
        samples[i] = pivot;
        sample_idxs[i] = partition_right;
    }

    /* Set up next iteration(s) if we have targets on either side. If the next
     * split is greater than the target, keep the current split and advance the
     * head of the slice. Else, advance the split and retract the tail of the
     * slice. */
    /* Targets less than pivot. */
    ret =
        distributed_quickselect_helper(arr, local_length, local_start, targets,
                samples, sample_idxs, geq_target_idx, left, partition_right);
    if (ret) {
        goto exit;
    }
    /* Targets greater than pivot. */
    ret =
        distributed_quickselect_helper(arr, local_length, local_start,
                targets + gt_target_idx, samples + gt_target_idx,
                sample_idxs + gt_target_idx, num_targets - gt_target_idx,
                partition_left, right);
    if (ret) {
        goto exit;
    }

exit:
    return ret;
}

/* Performs a distruted version of the quickselect algorithm to find NUM_TARGETS
 * target elements in TARGETS contained in ARR, which contains LOCAL_LENGTH
 * elements in the local, with the first element encrypted with index
 * LOCAL_START. Resulting samples are stored in SAMPLES. TARGETS must be a
 * sorted array. */
static int distributed_quickselect(elem_t *arr, size_t local_length,
        size_t local_start, size_t *targets, struct sample *samples,
        size_t *sample_idxs, size_t num_targets) {
    int ret =
        distributed_quickselect_helper(arr, local_length, local_start, targets,
                samples, sample_idxs, num_targets, 0, local_length);
    if (ret) {
        handle_error_string("Error in distributed quickselect");
        goto exit;
    }

exit:
    return ret;
}

/* Performs a non-oblivious samplesort across all enclaves. */
static int distributed_sample_partition(elem_t *arr, elem_t *out,
        size_t local_length, size_t local_start, size_t total_length) {
    size_t src_local_start = total_length * world_rank / world_size;
    size_t src_local_length =
        total_length * (world_rank + 1) / world_size - src_local_start;
    int ret;

    if (world_size == 1) {
        memcpy(out, arr, total_length * sizeof(*out));
        return 0;
    }

    /* Construct samples to and pass to quickselect. We want an even
     * distribution of elements across all ranks. */
    size_t sample_targets[world_size - 1];
    for (size_t i = 0; i < (size_t) world_size - 1; i++) {
        sample_targets[i] = total_length * (i + 1) / world_size;
    }
    struct sample samples[world_size - 1];
    size_t sample_idxs[world_size];
    size_t sample_scan_idxs[world_size];
    mpi_tls_request_t requests[world_size];
    size_t requests_len = world_size;
    ret =
        distributed_quickselect(arr, local_length, local_start, sample_targets,
                samples, sample_idxs, world_size - 1);
    if (ret) {
        handle_error_string("Error in distributed quickselect");
        goto exit;
    }
    memcpy(&sample_scan_idxs[1], sample_idxs,
            (world_size - 1) * sizeof(*sample_scan_idxs));
    sample_idxs[world_size - 1] = local_length;
    sample_scan_idxs[0] = 0;

    /* Send elements to their corresponding enclaves. The elements in the array
     * should already be partitioned. */

    /* Copy own partition's elements to the output. */
    size_t num_received =
        sample_idxs[world_rank] - sample_scan_idxs[world_rank];
    memcpy(out, arr + sample_scan_idxs[world_rank],
            num_received * sizeof(*out));
    sample_scan_idxs[world_rank] = sample_idxs[world_rank];

    /* Construct initial requests. REQUESTS is used for all send requests except
     * for REQUESTS[WORLD_RANK], which is our receive request. */
    for (int i = 0; i < world_size; i++) {
        if (i == world_rank) {
            size_t elems_to_recv =
                MIN(src_local_length - num_received, SAMPLE_PARTITION_BUF_SIZE);
            if (elems_to_recv > 0) {
                ret =
                    mpi_tls_irecv_bytes(out + num_received,
                            elems_to_recv * sizeof(*out), MPI_TLS_ANY_SOURCE,
                            SAMPLE_PARTITION_MPI_TAG, &requests[i]);
                if (ret) {
                    handle_error_string("Error receiving partitioned data");
                    goto exit;
                }
            } else {
                requests[i].type = MPI_TLS_NULL;
                requests_len--;
            }
        } else {
            if (sample_scan_idxs[i] < sample_idxs[i]) {
                size_t elems_to_send =
                    MIN(sample_idxs[i] - sample_scan_idxs[i],
                            SAMPLE_PARTITION_BUF_SIZE);

                /* Asynchronously send to enclave. */
                ret =
                    mpi_tls_isend_bytes(arr + sample_scan_idxs[i],
                            elems_to_send * sizeof(*arr), i,
                            SAMPLE_PARTITION_MPI_TAG, &requests[i]);
                if (ret) {
                    handle_error_string("Error sending partitioned data");
                    goto exit;
                }
                sample_scan_idxs[i] += elems_to_send;
            } else {
                requests[i].type = MPI_TLS_NULL;
                requests_len--;
            }
        }
    }

    /* Get completed requests in a loop. */
    while (requests_len) {
        size_t index;
        mpi_tls_status_t status;
        ret = mpi_tls_waitany(world_size, requests, &index, &status);
        if (ret) {
            handle_error_string("Error waiting on partition requests");
            goto exit;
        }
        bool keep_rank;
        if (index == (size_t) world_rank) {
            /* Receive request completed. */
            size_t req_num_received = status.count / sizeof(*out);
            num_received += req_num_received;

            size_t elems_to_recv =
                MIN(src_local_length - num_received, SAMPLE_PARTITION_BUF_SIZE);
            keep_rank = elems_to_recv > 0;
            if (keep_rank) {
                ret =
                    mpi_tls_irecv_bytes(out + num_received,
                            elems_to_recv * sizeof(*out), MPI_TLS_ANY_SOURCE,
                            SAMPLE_PARTITION_MPI_TAG, &requests[index]);
                if (ret) {
                    handle_error_string("Error receiving partitioned data");
                    goto exit;
                }
            }
        } else {
            /* Send request completed. */

            keep_rank =
                sample_idxs[index] - sample_scan_idxs[index] > 0;

            if (keep_rank) {
                size_t elems_to_send =
                    MIN(sample_idxs[index]
                                - sample_scan_idxs[index],
                            SAMPLE_PARTITION_BUF_SIZE);

                /* Asynchronously send to enclave. */
                ret =
                    mpi_tls_isend_bytes(arr + sample_scan_idxs[index],
                            elems_to_send * sizeof(*arr), index,
                            SAMPLE_PARTITION_MPI_TAG, &requests[index]);
                if (ret) {
                    handle_error_string("Error sending partitioned data");
                    goto exit;
                }
                sample_scan_idxs[index] += elems_to_send;
            }
        }

        if (!keep_rank) {
            /* Remove the request from the array. */
            requests[index].type = MPI_TLS_NULL;
            requests_len--;
        }
    }

    assert(num_received == src_local_length);

exit:
    return ret;
}

int nonoblivious_sort(elem_t *arr, size_t length, size_t local_length,
        size_t local_start) {
    size_t src_local_start = length * world_rank / world_size;
    size_t src_local_length =
        length * (world_rank + 1) / world_size - src_local_start;
    elem_t *buf = arr + local_length;
    int ret;

#ifdef DISTRIBUTED_SGX_SORT_BENCHMARK
    struct timespec time_start;
    if (clock_gettime(CLOCK_REALTIME, &time_start)) {
        handle_error_string("Error getting time");
        ret = errno;
        goto exit;
    }
#endif /* DISTRIBUTED_SGX_SORT_BENCHMARK */

    /* Partition permuted data such that each enclave has its own partition of
     * element, e.g. enclave 0 has the lowest elements, then enclave 1, etc. */
    ret =
        distributed_sample_partition(arr, buf, local_length, local_start,
                length);
    if (ret) {
        handle_error_string("Error in distributed sample partitioning");
        goto exit;
    }

#ifdef DISTRIBUTED_SGX_SORT_BENCHMARK
    struct timespec time_sample_partition;
    if (clock_gettime(CLOCK_REALTIME, &time_sample_partition)) {
        handle_error_string("Error getting time");
        ret = errno;
        goto exit;
    }
#endif /* DISTRIBUTED_SGX_SORT_BENCHMARK */

    /* Sort local partitions. */
    ret = mergesort(buf, arr, src_local_length);
    if (ret) {
        handle_error_string("Error in non-oblivious local sort");
        goto exit;
    }

#ifdef DISTRIBUTED_SGX_SORT_BENCHMARK
    struct timespec time_finish;
    if (clock_gettime(CLOCK_REALTIME, &time_finish)) {
        handle_error_string("Error getting time");
        ret = errno;
        goto exit;
    }
#endif /* DISTRIBUTED_SGX_SORT_BENCHMARK */

#ifdef DISTRIBUTED_SGX_SORT_BENCHMARK
    if (world_rank == 0) {
        printf("sample_partition : %f\n",
                get_time_difference(&time_start, &time_sample_partition));
        printf("local_sort       : %f\n",
                get_time_difference(&time_sample_partition, &time_finish));
    }
#endif

exit:
    return ret;
}
