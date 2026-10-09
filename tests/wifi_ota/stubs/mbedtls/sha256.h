#pragma once
#include <cstddef>
#include <cstdint>
// Toy checksum, NOT SHA or cryptographic verification; tests control API faults.
struct mbedtls_sha256_context { std::uint32_t sum; };
void mbedtls_sha256_init(mbedtls_sha256_context*);
void mbedtls_sha256_free(mbedtls_sha256_context*);
int mbedtls_sha256_starts(mbedtls_sha256_context*, int);
int mbedtls_sha256_update(mbedtls_sha256_context*, const unsigned char*, std::size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context*, unsigned char*);
int mbedtls_sha256(const unsigned char*, std::size_t, unsigned char*, int);
