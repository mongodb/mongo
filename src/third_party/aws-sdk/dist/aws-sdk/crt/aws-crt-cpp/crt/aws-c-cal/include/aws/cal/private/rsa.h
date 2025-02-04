#ifndef AWS_C_CAL_PRIVATE_RSA_H
#define AWS_C_CAL_PRIVATE_RSA_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/rsa.h>

#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>

struct aws_rsa_key_pair;
struct aws_der_decoder;

struct aws_rsa_key_vtable {
    int (*encrypt)(
        const struct aws_rsa_key_pair *key_pair,
        enum aws_rsa_encryption_algorithm algorithm,
        struct aws_byte_cursor plaintext,
        struct aws_byte_buf *out);
    int (*decrypt)(
        const struct aws_rsa_key_pair *key_pair,
        enum aws_rsa_encryption_algorithm algorithm,
        struct aws_byte_cursor ciphertext,
        struct aws_byte_buf *out);

    int (*sign)(
        const struct aws_rsa_key_pair *key_pair,
        enum aws_rsa_signature_algorithm algorithm,
        struct aws_byte_cursor digest,
        struct aws_byte_buf *out);

    int (*verify)(
        const struct aws_rsa_key_pair *key_pair,
        enum aws_rsa_signature_algorithm algorithm,
        struct aws_byte_cursor digest,
        struct aws_byte_cursor signature);
};

struct aws_rsa_key_pair {
    struct aws_allocator *allocator;
    struct aws_rsa_key_vtable *vtable;
    struct aws_ref_count ref_count;

    size_t key_size_in_bits;
    struct aws_byte_buf priv;
    struct aws_byte_buf pub;

    void *impl;
};

void aws_rsa_key_pair_base_clean_up(struct aws_rsa_key_pair *key_pair);

/*
 * RSAPrivateKey as defined in RFC 8017 (aka PKCS1 format):
 *   version           Version,
 *   modulus           INTEGER,  -- n
 *   publicExponent    INTEGER,  -- e
 *   privateExponent   INTEGER,  -- d
 *   prime1            INTEGER,  -- p
 *   prime2            INTEGER,  -- q
 *   exponent1         INTEGER,  -- d mod (p-1)
 *   exponent2         INTEGER,  -- d mod (q-1)
 *   coefficient       INTEGER,  -- (inverse of q) mod p
 *   otherPrimeInfos   OtherPrimeInfos OPTIONAL
 *   Note: otherPrimeInfos is used for >2 primes RSA cases, which are not very
 *   common and currently not supported by CRT. Version == 0 indicates 2 prime
 *   case and version == 1 indicates >2 prime case, hence in practice it will
 *   always be 0.
 */
struct aws_rsa_private_key_pkcs1 {
    /*
     * Note: all cursors here point to bignum data for underlying RSA numbers.
     * Struct itself does not own the data and points to where ever the data was
     * decoded from.
     */
    int version;
    struct aws_byte_cursor modulus;
    struct aws_byte_cursor publicExponent;
    struct aws_byte_cursor privateExponent;
    struct aws_byte_cursor prime1;
    struct aws_byte_cursor prime2;
    struct aws_byte_cursor exponent1;
    struct aws_byte_cursor exponent2;
    struct aws_byte_cursor coefficient;
};

AWS_CAL_API int aws_der_decoder_load_private_rsa_pkcs1(
    struct aws_der_decoder *decoder,
    struct aws_rsa_private_key_pkcs1 *out);

/*
* RSAPublicKey as defined in RFC 8017 (aka PKCS1 format):
    modulus           INTEGER,  -- n
    publicExponent    INTEGER   -- e
*/
struct aws_rsa_public_key_pkcs1 {
    /*
     * Note: all cursors here point to bignum data for underlying RSA numbers.
     * Struct itself does not own the data and points to where ever the data was
     * decoded from.
     */
    struct aws_byte_cursor modulus;
    struct aws_byte_cursor publicExponent;
};

AWS_CAL_API int aws_der_decoder_load_public_rsa_pkcs1(
    struct aws_der_decoder *decoder,
    struct aws_rsa_public_key_pkcs1 *out);

/*
 * Returns AWS_OP_SUCCESS if key size is supported and raises
 * AWS_ERROR_INVALID_ARGUMENT otherwise.
 */
int is_valid_rsa_key_size(size_t key_size_in_bits);

#endif /* AWS_C_CAL_PRIVATE_RSA_H */
