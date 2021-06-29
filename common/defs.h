#ifndef __DISTRIBUTED_SGX_SORT_COMMON_DEFS_H
#define __DISTRIBUTED_SGX_SORT_COMMON_DEFS_H

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CEIL_DIV(a, b) (((a) + (b) - 1) / (b))
#define UNUSED __attribute__((unused))

#endif /* distributed-sgx-sort/common/defs.h */
