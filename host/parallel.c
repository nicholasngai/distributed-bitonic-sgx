#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <mpi.h>
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
#include <openenclave/host.h>
#endif /* DISTRUBTED_SGX_SORT_HOSTONLY */
#include "common/crypto.h"
#include "common/defs.h"
#include "common/elem_t.h"
#include "common/error.h"
#include "common/ocalls.h"
#include "common/util.h"
#include "enclave/bucket.h"
#include "host/error.h"
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
#include "host/parallel_u.h"
#endif /* DISTRUBTED_SGX_SORT_HOSTONLY */

enum sort_type {
    SORT_BITONIC,
    SORT_BUCKET,
    SORT_OPAQUE,
};

enum ocall_mpi_request_type {
    OCALL_MPI_SEND,
    OCALL_MPI_RECV,
};

struct ocall_mpi_request {
    enum ocall_mpi_request_type type;
    void *buf;
    MPI_Request mpi_request;
};

static int world_rank;
static int world_size;

static unsigned char key[16];

static int init_mpi(int *argc, char ***argv) {
    int ret;

    /* Initialize MPI. */
    int threading_provided;
    ret = MPI_Init_thread(argc, argv, MPI_THREAD_MULTIPLE, &threading_provided);
    if (ret) {
        handle_mpi_error(ret, "MPI_Init_thread");
        goto exit;
    }
    if (threading_provided != MPI_THREAD_MULTIPLE) {
        printf("This program requires MPI_THREAD_MULTIPLE to be supported");
        ret = 1;
        goto exit;
    }

    /* Get world rank and size. */
    ret = MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    if (ret) {
        handle_mpi_error(ret, "MPI_Comm_rank");
        goto exit;
    }
    ret = MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    if (ret) {
        handle_mpi_error(ret, "MPI_Comm_size");
        goto exit;
    }

exit:
    return ret;
}

int ocall_mpi_send_bytes(const unsigned char *buf, size_t count, int dest,
        int tag) {
    if (count > INT_MAX) {
        handle_error_string("Count too large");
        return MPI_ERR_COUNT;
    }

    return MPI_Send(buf, (int) count, MPI_UNSIGNED_CHAR, dest, tag,
            MPI_COMM_WORLD);
}

int ocall_mpi_recv_bytes(unsigned char *buf, size_t count, int source,
        int tag, ocall_mpi_status_t *status) {
    int ret;

    if (count > INT_MAX) {
        handle_error_string("Count too large");
        ret = MPI_ERR_COUNT;
        goto exit;
    }

    if (source == OCALL_MPI_ANY_SOURCE) {
        source = MPI_ANY_SOURCE;
    }
    if (tag == OCALL_MPI_ANY_TAG) {
        tag = MPI_ANY_TAG;
    }

    MPI_Status mpi_status;
    ret = MPI_Recv(buf, (int) count, MPI_UNSIGNED_CHAR, source, tag,
            MPI_COMM_WORLD, &mpi_status);

    /* Populate status. */
    ret = MPI_Get_count(&mpi_status, MPI_UNSIGNED_CHAR, &status->count);
    if (ret) {
        handle_mpi_error(ret, "MPI_Get_count");
        goto exit;
    }
    status->source = mpi_status.MPI_SOURCE;
    status->tag = mpi_status.MPI_TAG;

exit:
    return ret;
}

int ocall_mpi_try_recv_bytes(unsigned char *buf, size_t count, int source,
        int tag, int *flag, ocall_mpi_status_t *status) {
    if (count > INT_MAX) {
        handle_error_string("Count too large");
        return -1;
    }

    MPI_Status mpi_status;
    int ret;

    if (source == OCALL_MPI_ANY_SOURCE) {
        source = MPI_ANY_SOURCE;
    }
    if (tag == OCALL_MPI_ANY_TAG) {
        tag = MPI_ANY_TAG;
    }

    /* Probe for an available message. */
    ret = MPI_Iprobe(source, tag, MPI_COMM_WORLD, flag, &mpi_status);
    if (ret) {
        handle_mpi_error(ret, "MPI_Probe");
        goto exit;
    }
    if (!*flag) {
        goto exit;
    }

    /* Get incoming message parameters. */
    int bytes_to_recv;
    ret = MPI_Get_count(&mpi_status, MPI_UNSIGNED_CHAR, &bytes_to_recv);
    if (ret) {
        handle_mpi_error(ret, "MPI_Get_count");
        goto exit;
    }
    source = mpi_status.MPI_SOURCE;
    tag = mpi_status.MPI_TAG;

    /* Read in that number of bytes. */
    ret = MPI_Recv(buf, count, MPI_UNSIGNED_CHAR, source, tag,
            MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (ret) {
        handle_mpi_error(ret, "MPI_Recv");
        goto exit;
    }

    /* Populate status. */
    status->count = bytes_to_recv;
    status->source = source;
    status->tag = tag;

exit:
    return ret;
}

int ocall_mpi_isend_bytes(const unsigned char *buf, size_t count, int dest,
        int tag, ocall_mpi_request_t *request) {
    int ret;

    if (count > INT_MAX) {
        handle_error_string("Count too large");
        return MPI_ERR_COUNT;
    }

    /* Allocate request. */
    *request = malloc(sizeof(**request));
    if (!*request) {
        perror("malloc ocall_mpi_request");
        ret = errno;
        goto exit;
    }
    (*request)->type = OCALL_MPI_SEND;
    (*request)->buf = malloc(count);
    if (!(*request)->buf) {
        perror("malloc isend buf");
        ret = errno;
        goto exit_free_request;
    }

    /* Copy bytes to send to permanent buffer. */
    memcpy((*request)->buf, buf, count);

    /* Start request. */
    ret = MPI_Isend((*request)->buf, (int) count, MPI_UNSIGNED_CHAR, dest, tag,
            MPI_COMM_WORLD, &(*request)->mpi_request);
    if (ret) {
        handle_mpi_error(ret, "MPI_Isend");
        goto exit_free_buf;
    }

    ret = 0;

    return ret;

exit_free_buf:
    free((*request)->buf);
exit_free_request:
    free(*request);
exit:
    return ret;
}

int ocall_mpi_irecv_bytes(size_t count, int source, int tag,
        ocall_mpi_request_t *request) {
    int ret;

    if (count > INT_MAX) {
        handle_error_string("Count too large");
        return MPI_ERR_COUNT;
    }

    if (source == OCALL_MPI_ANY_SOURCE) {
        source = MPI_ANY_SOURCE;
    }
    if (tag == OCALL_MPI_ANY_TAG) {
        tag = MPI_ANY_TAG;
    }

    /* Allocate request. */
    *request = malloc(sizeof(**request));
    if (!*request) {
        perror("malloc ocall_mpi_request");
        ret = errno;
        goto exit;
    }
    (*request)->type = OCALL_MPI_RECV;
    (*request)->buf = malloc(count);
    if (!(*request)->buf) {
        perror("malloc irecv buf");
        ret = errno;
        goto exit_free_request;
    }

    /* Start request. */
    ret = MPI_Irecv((*request)->buf, (int) count, MPI_UNSIGNED_CHAR, source,
            tag, MPI_COMM_WORLD, &(*request)->mpi_request);
    if (ret) {
        handle_mpi_error(ret, "MPI_Irecv");
        goto exit_free_buf;
    }

    ret = 0;

    return ret;

exit_free_buf:
    free((*request)->buf);
exit_free_request:
    free(*request);
exit:
    return ret;
}

int ocall_mpi_wait(unsigned char *buf, size_t count,
        ocall_mpi_request_t *request, ocall_mpi_status_t *status) {
    int ret;

    MPI_Request mpi_request;
    if (*request == OCALL_MPI_REQUEST_NULL) {
        mpi_request = MPI_REQUEST_NULL;
    } else {
        mpi_request = (*request)->mpi_request;
    }

    MPI_Status mpi_status;
    ret = MPI_Wait(&mpi_request, &mpi_status);
    if (ret) {
        handle_mpi_error(ret, "MPI_Wait");
        goto exit_free_request;
    }

    switch ((*request)->type) {
    case OCALL_MPI_SEND:
        break;

    case OCALL_MPI_RECV:
        /* Populate status. */
        ret = MPI_Get_count(&mpi_status, MPI_UNSIGNED_CHAR, &status->count);
        if (ret) {
            handle_mpi_error(ret, "MPI_Get_count");
            goto exit_free_request;
        }
        status->source = mpi_status.MPI_SOURCE;
        status->tag = mpi_status.MPI_TAG;

        /* Copy bytes to output. */
        memcpy(buf, (*request)->buf, MIN(count, (size_t) status->count));

        break;
    }

exit_free_request:
    free((*request)->buf);
    free(*request);
    return ret;
}

int ocall_mpi_waitany(unsigned char *buf, size_t bufcount, size_t count,
        ocall_mpi_request_t *requests, size_t *index,
        ocall_mpi_status_t *status) {
    int ret;

    MPI_Request mpi_requests[count];
    for (size_t i = 0; i < count; i++) {
        if (requests[i] == OCALL_MPI_REQUEST_NULL) {
            mpi_requests[i] = MPI_REQUEST_NULL;
        } else {
            mpi_requests[i] = requests[i]->mpi_request;
        }
    }

    MPI_Status mpi_status;
    int mpi_index;
    ret = MPI_Waitany(count, mpi_requests, &mpi_index, &mpi_status);
    if (ret) {
        handle_mpi_error(ret, "MPI_Waitany");
        goto exit_free_request;
    }
    if (mpi_index == MPI_UNDEFINED) {
        ret = -1;
        handle_error_string("All null requests passed to ocall_mpi_waitany");
        goto exit;
    }
    *index = mpi_index;

    switch (requests[*index]->type) {
    case OCALL_MPI_SEND:
        break;

    case OCALL_MPI_RECV:
        /* Populate status. */
        ret = MPI_Get_count(&mpi_status, MPI_UNSIGNED_CHAR, &status->count);
        if (ret) {
            handle_mpi_error(ret, "MPI_Get_count");
            goto exit_free_request;
        }
        status->source = mpi_status.MPI_SOURCE;
        status->tag = mpi_status.MPI_TAG;

        /* Copy bytes to output. */
        memcpy(buf, requests[*index]->buf, MIN(bufcount,
                    (size_t) status->count));

        break;
    }

exit_free_request:
    free(requests[*index]->buf);
    free(requests[*index]);
exit:
    return ret;
}

int ocall_mpi_try_wait(unsigned char *buf, size_t count,
        ocall_mpi_request_t *request, int *flag, ocall_mpi_status_t *status) {
    int ret;

    MPI_Status mpi_status;

    /* Test request status. */
    ret = MPI_Test(&(*request)->mpi_request, flag, &mpi_status);
    if (ret) {
        handle_mpi_error(ret, "MPI_Test");
        goto exit_free_request;
    }
    if (!*flag) {
        goto exit;
    }

    switch ((*request)->type) {
    case OCALL_MPI_SEND:
        break;

    case OCALL_MPI_RECV:
        /* Populate status. */
        ret = MPI_Get_count(&mpi_status, MPI_UNSIGNED_CHAR, &status->count);
        if (ret) {
            handle_mpi_error(ret, "MPI_Get_count");
            goto exit_free_request;
        }
        status->source = mpi_status.MPI_SOURCE;
        status->tag = mpi_status.MPI_TAG;

        /* Copy bytes to output. */
        memcpy(buf, (*request)->buf, MIN(count, (size_t) status->count));

        break;
    }

exit_free_request:
    free((*request)->buf);
    free(*request);
exit:
    return ret;
}

int ocall_mpi_cancel(ocall_mpi_request_t *request) {
    int ret;

    ret = MPI_Cancel(&(*request)->mpi_request);
    if (ret) {
        handle_mpi_error(ret, "MPI_Cancel");
        goto exit_free_request;
    }

exit_free_request:
    free((*request)->buf);
    free(*request);
    return ret;
}

void ocall_mpi_barrier(void) {
    MPI_Barrier(MPI_COMM_WORLD);
}

static void *start_thread_work(void *enclave_) {
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    oe_enclave_t *enclave = enclave_;
    oe_result_t result = ecall_start_work(enclave);
    if (result != OE_OK) {
        handle_oe_error(result, "ecall_start_work");
    }
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    ecall_start_work();
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    return 0;
}

int main(int argc, char **argv) {
    int ret = -1;
    size_t num_threads = 1;

    /* Read arguments. */

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    if (argc < 4) {
        printf("usage: %s enclave_image {bitonic|bucket|opaque} array_size [num_threads]\n", argv[0]);
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    if (argc < 3) {
        printf("usage: %s {bitonic|bucket|opaque} array_size [num_threads]\n", argv[0]);
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        return 0;
    }

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
#define SORT_TYPE_STR (argv[2])
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
#define SORT_TYPE_STR (argv[1])
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    enum sort_type sort_type;
    if (strcmp(SORT_TYPE_STR, "bitonic") == 0) {
        sort_type = SORT_BITONIC;
    } else if (strcmp(SORT_TYPE_STR, "bucket") == 0) {
        sort_type = SORT_BUCKET;
    } else if (strcmp(SORT_TYPE_STR, "opaque") == 0) {
        sort_type = SORT_OPAQUE;
    } else {
        printf("Invalid sort type\n");
        return ret;
    }
#undef SORT_TYPE_STR

    size_t length;
    {
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
        ssize_t l = atoll(argv[3]);
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        ssize_t l = atoll(argv[2]);
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        if (l < 0) {
            printf("Invalid array size\n");
            return ret;
        }
        length = l;
    }

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    if (argc >= 5) {
        ssize_t n = atoll(argv[4]);
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    if (argc >= 4) {
        ssize_t n = atoll(argv[3]);
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        if (n < 0) {
            printf("Invalid number of threads\n");
            return ret;
        }
        num_threads = n;

        if (sort_type == SORT_OPAQUE && n > 1) {
            printf("Opaque sort does not support more than 1 thread\n");
            return ret;
        }
    }

    pthread_t threads[num_threads - 1];

    /* Init MPI. */

    ret = init_mpi(&argc, &argv);

    /* Create enclave. */

    if (ret) {
        handle_error_string("init_mpi");
        goto exit_mpi_finalize;
    }

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    oe_enclave_t *enclave;
    oe_result_t result;
    int flags = 0;
#ifdef OE_DEBUG
    flags |= OE_ENCLAVE_FLAG_DEBUG;
#endif
#ifdef OE_SIMULATION
    flags |= OE_ENCLAVE_FLAG_SIMULATE;
#endif
    result = oe_create_parallel_enclave(
            argv[1],
            OE_ENCLAVE_TYPE_AUTO,
            flags,
            NULL,
            0,
            &enclave);

    if (result != OE_OK) {
        handle_oe_error(result, "oe_create_parallel_enclave");
        ret = result;
        goto exit_mpi_finalize;
    }
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */

    /* Init enclave with threads. */

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    result = ecall_set_params(enclave, world_rank, world_size, num_threads);
    if (result != OE_OK) {
        handle_oe_error(result, "ecall_set_params");
        goto exit_terminate_enclave;
    }
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    ecall_set_params(world_rank, world_size, num_threads);
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */

    for (size_t i = 1; i < num_threads; i++) {
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
        ret = pthread_create(&threads[i - 1], NULL, start_thread_work, enclave);
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        ret = pthread_create(&threads[i - 1], NULL, start_thread_work, NULL);
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
        if (ret) {
            perror("pthread_create");
            goto exit_terminate_enclave;
        }
    }

    /* Init random array. */

    size_t local_length =
        ((world_rank + 1) * length + world_size - 1) / world_size
            - (world_rank * length + world_size - 1) / world_size;
    size_t local_start = (world_rank * length + world_size - 1) / world_size;
    unsigned char *arr;
    switch (sort_type) {
        case SORT_BITONIC:
            arr = malloc(local_length * SIZEOF_ENCRYPTED_NODE);
            break;
        case SORT_BUCKET: {
            /* The total number of buckets is the max of either double the
             * number of buckets needed to hold all the elements or double the
             * number of enclaves (since each enclaves needs at least two
             * buckets. */
            size_t num_buckets =
                MAX(next_pow2l(length) * 2 / BUCKET_SIZE, world_size * 2);
            size_t local_num_buckets =
                num_buckets * (world_rank + 1) / world_size
                    - num_buckets * world_rank / world_size;
            /* The bucket sort relies on having 2 local buffers, so we allocate
             * double the size of a single buffer (a single buffer is
             * local_num_buckets * BUCKET_SIZE * SIZEOF_ENCRYPTED_NODE). */
            arr =
                malloc(local_num_buckets * BUCKET_SIZE
                        * SIZEOF_ENCRYPTED_NODE * 2);
            break;
        case SORT_OPAQUE:
            arr = malloc(local_length * 2 * SIZEOF_ENCRYPTED_NODE);
            break;
        }
    }
    if (!arr) {
        perror("malloc arr");
        goto exit_terminate_enclave;
    }
    if (entropy_init()) {
        handle_error_string("Error initializing host entropy context");
        goto exit_free_arr;
    }
    if (rand_init()) {
        handle_error_string("Error initializing host random number generator");
        entropy_free();
        goto exit_free_arr;
    }
    srand(world_rank + 1);
    for (size_t i = local_start; i < local_start + local_length; i++) {
        /* Initialize elem. */
        elem_t elem;
        memset(&elem, '\0', sizeof(elem));
        elem.key = rand();

        /* Encrypt to array. */
        unsigned char *start = arr + (i - local_start) * SIZEOF_ENCRYPTED_NODE;
        ret = elem_encrypt(key, &elem, start, i);
        if (ret < 0) {
            handle_error_string("Error encrypting elem in host");
        }
    }
    rand_free();
    entropy_free();

    /* Time sort and join. */

    struct timespec start;
    ret = timespec_get(&start, TIME_UTC);
    if (!ret) {
        perror("starting timespec_get");
        goto exit_free_arr;
    }

#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    switch (sort_type) {
        case SORT_BITONIC:
            result = ecall_bitonic_sort(enclave, &ret, arr, length, local_length);
            break;
        case SORT_BUCKET:
            result = ecall_bucket_sort(enclave, &ret, arr, length, local_length);
            break;
        case SORT_OPAQUE:
            result = ecall_opaque_sort(enclave, &ret, arr, length, local_length);
            break;
    }
    if (result != OE_OK) {
        goto exit_free_arr;
    }
#else /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    switch (sort_type) {
        case SORT_BITONIC:
            ret = ecall_bitonic_sort(arr, length, local_length);
            break;
        case SORT_BUCKET:
            ret = ecall_bucket_sort(arr, length, local_length);
            break;
        case SORT_OPAQUE:
            ret = ecall_opaque_sort(arr, length, local_length);
            break;
    }
#endif /* DISTRIBUTED_SGX_SORT_HOSTONLY */
    if (ret) {
        goto exit_free_arr;
    }

    for (size_t i = 1; i < num_threads; i++) {
        pthread_join(threads[i - 1], NULL);
    }

    if (ret) {
        handle_error_string("Enclave exited with return code %d", ret);
        goto exit_terminate_enclave;
    }

    MPI_Barrier(MPI_COMM_WORLD);

    struct timespec end;
    ret = timespec_get(&end, TIME_UTC);
    if (!ret) {
        perror("ending timespec_get");
        goto exit_free_arr;
    }

    /* Check array. */

    uint64_t first_key = 0;
    uint64_t prev_key = 0;
    for (int rank = 0; rank < world_size; rank++) {
        if (rank == world_rank) {
            for (size_t i = local_start; i < local_start + local_length; i++) {
                /* Decrypt elem. */
                elem_t elem;
                unsigned char *start = arr + (i - local_start) * SIZEOF_ENCRYPTED_NODE;
                ret = elem_decrypt(key, &elem, start, i);
                if (ret < 0) {
                    handle_error_string("Error decrypting elem in host");
                }
                //printf("%d: %lu\n", world_rank, elem.key);
                if (i == local_start) {
                   first_key = elem.key;
                } else if (prev_key > elem.key) {
                    printf("Not sorted correctly!\n");
                    break;
                }
                prev_key = elem.key;
            }
        }
        MPI_Barrier(MPI_COMM_WORLD);
    }

    if (world_rank < world_size - 1) {
        /* Send largest value to next elem. prev_key now contains the last item
         * in the array. */
        MPI_Send(&prev_key, 1, MPI_UNSIGNED_LONG_LONG, world_rank + 1, 0,
                MPI_COMM_WORLD);
    }

    if (world_rank > 0) {
        /* Receive previous elem's largest value and compare. */
        MPI_Recv(&prev_key, 1, MPI_UNSIGNED_LONG_LONG, world_rank - 1, 0,
                MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (prev_key > first_key) {
            printf("Not sorted correctly at enclave boundaries!\n");
        }
    }

    /* Print time taken. */

    if (world_rank == 0) {
        double seconds_taken =
            (double) ((end.tv_sec * 1000000000 + end.tv_nsec)
                    - (start.tv_sec * 1000000000 + start.tv_nsec))
            / 1000000000;
        printf("%f\n", seconds_taken);
    }

exit_free_arr:
    free(arr);
exit_terminate_enclave:
#ifndef DISTRIBUTED_SGX_SORT_HOSTONLY
    oe_terminate_enclave(enclave);
#endif
exit_mpi_finalize:
    MPI_Finalize();
    return ret;
}
