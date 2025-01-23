/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/ecc.h>

#include <aws/cal/cal.h>
#include <aws/cal/private/der.h>

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

struct libcrypto_ecc_key {
    struct aws_ecc_key_pair key_pair;
    EC_KEY *ec_key;
};

static int s_curve_name_to_nid(enum aws_ecc_curve_name curve_name) {
    switch (curve_name) {
        case AWS_CAL_ECDSA_P256:
            return NID_X9_62_prime256v1;
        case AWS_CAL_ECDSA_P384:
            return NID_secp384r1;
    }

    AWS_FATAL_ASSERT(!"Unsupported elliptic curve name");
    return -1;
}

static void s_key_pair_destroy(struct aws_ecc_key_pair *key_pair) {

    if (key_pair) {
        aws_byte_buf_clean_up(&key_pair->pub_x);
        aws_byte_buf_clean_up(&key_pair->pub_y);
        aws_byte_buf_clean_up_secure(&key_pair->priv_d);

        struct libcrypto_ecc_key *key_impl = key_pair->impl;

        if (key_impl->ec_key) {
            EC_KEY_free(key_impl->ec_key);
        }
        aws_mem_release(key_pair->allocator, key_pair);
    }
}

static int s_sign_payload(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *hash,
    struct aws_byte_buf *signature_output) {
    struct libcrypto_ecc_key *libcrypto_key_pair = key_pair->impl;

    unsigned int signature_size = signature_output->capacity - signature_output->len;
    int ret_val = ECDSA_sign(
        0,
        hash->ptr,
        hash->len,
        signature_output->buffer + signature_output->len,
        &signature_size,
        libcrypto_key_pair->ec_key);
    signature_output->len += signature_size;

    return ret_val == 1 ? AWS_OP_SUCCESS : aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

static int s_verify_payload(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *hash,
    const struct aws_byte_cursor *signature) {
    struct libcrypto_ecc_key *libcrypto_key_pair = key_pair->impl;

    return ECDSA_verify(0, hash->ptr, hash->len, signature->ptr, signature->len, libcrypto_key_pair->ec_key) == 1
               ? AWS_OP_SUCCESS
               : aws_raise_error(AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED);
}

static size_t s_signature_length(const struct aws_ecc_key_pair *key_pair) {
    struct libcrypto_ecc_key *libcrypto_key_pair = key_pair->impl;

    return ECDSA_size(libcrypto_key_pair->ec_key);
}

static int s_fill_in_public_key_info(
    struct libcrypto_ecc_key *libcrypto_key_pair,
    const EC_GROUP *group,
    const EC_POINT *pub_key_point) {
    BIGNUM *big_num_x = BN_new();
    BIGNUM *big_num_y = BN_new();

    int ret_val = AWS_OP_ERR;

    if (EC_POINT_get_affine_coordinates_GFp(group, pub_key_point, big_num_x, big_num_y, NULL) != 1) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto clean_up;
    }

    size_t x_coor_size = BN_num_bytes(big_num_x);
    size_t y_coor_size = BN_num_bytes(big_num_y);

    if (aws_byte_buf_init(&libcrypto_key_pair->key_pair.pub_x, libcrypto_key_pair->key_pair.allocator, x_coor_size)) {
        goto clean_up;
    }

    if (aws_byte_buf_init(&libcrypto_key_pair->key_pair.pub_y, libcrypto_key_pair->key_pair.allocator, y_coor_size)) {
        goto clean_up;
    }

    BN_bn2bin(big_num_x, libcrypto_key_pair->key_pair.pub_x.buffer);
    BN_bn2bin(big_num_y, libcrypto_key_pair->key_pair.pub_y.buffer);

    libcrypto_key_pair->key_pair.pub_x.len = x_coor_size;
    libcrypto_key_pair->key_pair.pub_y.len = y_coor_size;

    ret_val = AWS_OP_SUCCESS;

clean_up:
    BN_free(big_num_x);
    BN_free(big_num_y);

    return ret_val;
}

static int s_derive_public_key(struct aws_ecc_key_pair *key_pair) {
    struct libcrypto_ecc_key *libcrypto_key_pair = key_pair->impl;

    if (!libcrypto_key_pair->key_pair.priv_d.buffer) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* we already have a public key. */
    if (libcrypto_key_pair->key_pair.pub_x.len) {
        return AWS_OP_SUCCESS;
    }

    BIGNUM *priv_key_num =
        BN_bin2bn(libcrypto_key_pair->key_pair.priv_d.buffer, libcrypto_key_pair->key_pair.priv_d.len, NULL);

    const EC_GROUP *group = EC_KEY_get0_group(libcrypto_key_pair->ec_key);
    EC_POINT *point = EC_POINT_new(group);

    EC_POINT_mul(group, point, priv_key_num, NULL, NULL, NULL);
    BN_free(priv_key_num);

    EC_KEY_set_public_key(libcrypto_key_pair->ec_key, point);
    int ret_val = s_fill_in_public_key_info(libcrypto_key_pair, group, point);
    EC_POINT_free(point);
    return ret_val;
}

static struct aws_ecc_key_pair_vtable vtable = {
    .sign_message = s_sign_payload,
    .verify_signature = s_verify_payload,
    .derive_pub_key = s_derive_public_key,
    .signature_length = s_signature_length,
    .destroy = s_key_pair_destroy,
};

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_private_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *priv_key) {

    size_t key_length = aws_ecc_key_coordinate_byte_size_from_curve_name(curve_name);
    if (priv_key->len != key_length) {
        AWS_LOGF_ERROR(AWS_LS_CAL_ECC, "Private key length does not match curve's expected length");
        aws_raise_error(AWS_ERROR_CAL_INVALID_KEY_LENGTH_FOR_ALGORITHM);
        return NULL;
    }

    struct libcrypto_ecc_key *key_impl = aws_mem_calloc(allocator, 1, sizeof(struct libcrypto_ecc_key));

    key_impl->ec_key = EC_KEY_new_by_curve_name(s_curve_name_to_nid(curve_name));
    key_impl->key_pair.curve_name = curve_name;
    key_impl->key_pair.allocator = allocator;
    key_impl->key_pair.vtable = &vtable;
    key_impl->key_pair.impl = key_impl;
    aws_atomic_init_int(&key_impl->key_pair.ref_count, 1);
    aws_byte_buf_init_copy_from_cursor(&key_impl->key_pair.priv_d, allocator, *priv_key);

    BIGNUM *priv_key_num = BN_bin2bn(key_impl->key_pair.priv_d.buffer, key_impl->key_pair.priv_d.len, NULL);
    if (!EC_KEY_set_private_key(key_impl->ec_key, priv_key_num)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_ECC, "Failed to set openssl private key");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        BN_free(priv_key_num);
        s_key_pair_destroy(&key_impl->key_pair);
        return NULL;
    }
    BN_free(priv_key_num);
    return &key_impl->key_pair;
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_generate_random(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name) {
    struct libcrypto_ecc_key *key_impl = aws_mem_calloc(allocator, 1, sizeof(struct libcrypto_ecc_key));

    key_impl->ec_key = EC_KEY_new_by_curve_name(s_curve_name_to_nid(curve_name));
    key_impl->key_pair.curve_name = curve_name;
    key_impl->key_pair.allocator = allocator;
    key_impl->key_pair.vtable = &vtable;
    key_impl->key_pair.impl = key_impl;
    aws_atomic_init_int(&key_impl->key_pair.ref_count, 1);

    if (EC_KEY_generate_key(key_impl->ec_key) != 1) {
        goto error;
    }

    const EC_POINT *pub_key_point = EC_KEY_get0_public_key(key_impl->ec_key);
    const EC_GROUP *group = EC_KEY_get0_group(key_impl->ec_key);

    const BIGNUM *private_key_num = EC_KEY_get0_private_key(key_impl->ec_key);
    size_t priv_key_size = BN_num_bytes(private_key_num);
    if (aws_byte_buf_init(&key_impl->key_pair.priv_d, allocator, priv_key_size)) {
        goto error;
    }

    BN_bn2bin(private_key_num, key_impl->key_pair.priv_d.buffer);
    key_impl->key_pair.priv_d.len = priv_key_size;

    if (!s_fill_in_public_key_info(key_impl, group, pub_key_point)) {
        return &key_impl->key_pair;
    }

error:
    s_key_pair_destroy(&key_impl->key_pair);
    return NULL;
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_public_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *public_key_x,
    const struct aws_byte_cursor *public_key_y) {
    struct libcrypto_ecc_key *key_impl = aws_mem_calloc(allocator, 1, sizeof(struct libcrypto_ecc_key));
    BIGNUM *pub_x_num = NULL;
    BIGNUM *pub_y_num = NULL;
    EC_POINT *point = NULL;

    if (!key_impl) {
        return NULL;
    }

    key_impl->ec_key = EC_KEY_new_by_curve_name(s_curve_name_to_nid(curve_name));
    key_impl->key_pair.curve_name = curve_name;
    key_impl->key_pair.allocator = allocator;
    key_impl->key_pair.vtable = &vtable;
    key_impl->key_pair.impl = key_impl;
    aws_atomic_init_int(&key_impl->key_pair.ref_count, 1);

    if (aws_byte_buf_init_copy_from_cursor(&key_impl->key_pair.pub_x, allocator, *public_key_x)) {
        s_key_pair_destroy(&key_impl->key_pair);
        return NULL;
    }

    if (aws_byte_buf_init_copy_from_cursor(&key_impl->key_pair.pub_y, allocator, *public_key_y)) {
        s_key_pair_destroy(&key_impl->key_pair);
        return NULL;
    }

    pub_x_num = BN_bin2bn(public_key_x->ptr, public_key_x->len, NULL);
    pub_y_num = BN_bin2bn(public_key_y->ptr, public_key_y->len, NULL);

    const EC_GROUP *group = EC_KEY_get0_group(key_impl->ec_key);
    point = EC_POINT_new(group);

    if (EC_POINT_set_affine_coordinates_GFp(group, point, pub_x_num, pub_y_num, NULL) != 1) {
        goto error;
    }

    if (EC_KEY_set_public_key(key_impl->ec_key, point) != 1) {
        goto error;
    }

    EC_POINT_free(point);
    BN_free(pub_x_num);
    BN_free(pub_y_num);

    return &key_impl->key_pair;

error:
    if (point) {
        EC_POINT_free(point);
    }

    if (pub_x_num) {
        BN_free(pub_x_num);
    }

    if (pub_y_num) {
        BN_free(pub_y_num);
    }

    s_key_pair_destroy(&key_impl->key_pair);

    return NULL;
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_asn1(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *encoded_keys) {

    struct aws_ecc_key_pair *key = NULL;
    struct aws_der_decoder *decoder = aws_der_decoder_new(allocator, *encoded_keys);

    if (!decoder) {
        return NULL;
    }

    struct aws_byte_cursor pub_x;
    struct aws_byte_cursor pub_y;
    struct aws_byte_cursor priv_d;

    enum aws_ecc_curve_name curve_name;
    if (aws_der_decoder_load_ecc_key_pair(decoder, &pub_x, &pub_y, &priv_d, &curve_name)) {
        goto error;
    }

    if (priv_d.ptr) {
        struct libcrypto_ecc_key *key_impl = aws_mem_calloc(allocator, 1, sizeof(struct libcrypto_ecc_key));
        key_impl->key_pair.curve_name = curve_name;
        /* as awkward as it seems, there's not a great way to manually set the public key, so let openssl just parse
         * the der document manually now that we know what parts are what. */
        if (!d2i_ECPrivateKey(&key_impl->ec_key, (const unsigned char **)&encoded_keys->ptr, encoded_keys->len)) {
            aws_mem_release(allocator, key_impl);
            aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
            goto error;
        }

        key_impl->key_pair.allocator = allocator;
        key_impl->key_pair.vtable = &vtable;
        key_impl->key_pair.impl = key_impl;
        aws_atomic_init_int(&key_impl->key_pair.ref_count, 1);
        key = &key_impl->key_pair;

        struct aws_byte_buf temp_buf;
        AWS_ZERO_STRUCT(temp_buf);

        if (pub_x.ptr) {
            temp_buf = aws_byte_buf_from_array(pub_x.ptr, pub_x.len);
            if (aws_byte_buf_init_copy(&key->pub_x, allocator, &temp_buf)) {
                goto error;
            }
        }

        if (pub_y.ptr) {
            temp_buf = aws_byte_buf_from_array(pub_y.ptr, pub_y.len);
            if (aws_byte_buf_init_copy(&key->pub_y, allocator, &temp_buf)) {
                goto error;
            }
        }

        if (priv_d.ptr) {
            temp_buf = aws_byte_buf_from_array(priv_d.ptr, priv_d.len);
            if (aws_byte_buf_init_copy(&key->priv_d, allocator, &temp_buf)) {
                goto error;
            }
        }

    } else {
        key = aws_ecc_key_pair_new_from_public_key(allocator, curve_name, &pub_x, &pub_y);

        if (!key) {
            goto error;
        }
    }

    aws_der_decoder_destroy(decoder);
    return key;

error:
    aws_der_decoder_destroy(decoder);
    s_key_pair_destroy(key);

    return NULL;
}
