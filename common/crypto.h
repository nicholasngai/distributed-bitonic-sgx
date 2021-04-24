#ifndef __DISTRIBUTED_SGX_SORT_COMMON_CRYPTO_H
#define __DISTRIBUTED_SGX_SORT_COMMON_CRYPTO_H

#include <stddef.h>
#include <mbedtls/entropy.h>

#define IV_LEN 12
#define TAG_LEN 16

extern mbedtls_entropy_context entropy_ctx;

int entropy_init(void);
void entropy_free(void);
int rand_init(void);
void rand_free(void);
int rand_read(void *buf, size_t n);

int aad_encrypt(void *key, void *plaintext, size_t plaintext_len, void *aad,
        size_t aad_len, void *iv, void *ciphertext, void *tag);
int aad_decrypt(void *key, void *ciphertext, size_t ciphertext_len, void *aad,
        size_t aad_len, void *iv, void *tag, void *plaintext);

#endif /* distributed-sgx-sort/common/crypto.h */