/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/ecc.h>

#include <aws/cal/cal.h>
#include <aws/cal/private/der.h>
#include <aws/common/encoding.h>

#define STATIC_INIT_BYTE_CURSOR(a, name)                                                                               \
    static struct aws_byte_cursor s_##name = {                                                                         \
        .ptr = (a),                                                                                                    \
        .len = sizeof(a),                                                                                              \
    };

static uint8_t s_p256_oid[] = {
    0x2A,
    0x86,
    0x48,
    0xCE,
    0x3D,
    0x03,
    0x01,
    0x07,
};
STATIC_INIT_BYTE_CURSOR(s_p256_oid, ecc_p256_oid)

static uint8_t s_p384_oid[] = {
    0x2B,
    0x81,
    0x04,
    0x00,
    0x22,
};
STATIC_INIT_BYTE_CURSOR(s_p384_oid, ecc_p384_oid)

static struct aws_byte_cursor *s_ecc_curve_oids[] = {
    [AWS_CAL_ECDSA_P256] = &s_ecc_p256_oid,
    [AWS_CAL_ECDSA_P384] = &s_ecc_p384_oid,
};

int aws_ecc_curve_name_from_oid(struct aws_byte_cursor *oid, enum aws_ecc_curve_name *curve_name) {
    if (aws_byte_cursor_eq(oid, &s_ecc_p256_oid)) {
        *curve_name = AWS_CAL_ECDSA_P256;
        return AWS_OP_SUCCESS;
    }

    if (aws_byte_cursor_eq(oid, &s_ecc_p384_oid)) {
        *curve_name = AWS_CAL_ECDSA_P384;
        return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_CAL_UNKNOWN_OBJECT_IDENTIFIER);
}

int aws_ecc_oid_from_curve_name(enum aws_ecc_curve_name curve_name, struct aws_byte_cursor *oid) {
    if (curve_name < AWS_CAL_ECDSA_P256 || curve_name > AWS_CAL_ECDSA_P384) {
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
    }
    *oid = *s_ecc_curve_oids[curve_name];
    return AWS_OP_SUCCESS;
}

typedef struct aws_ecc_key_pair *(aws_ecc_key_pair_new_from_public_key_fn)(struct aws_allocator *allocator,
                                                                           enum aws_ecc_curve_name curve_name,
                                                                           const struct aws_byte_cursor *public_key_x,
                                                                           const struct aws_byte_cursor *public_key_y);

typedef struct aws_ecc_key_pair *(aws_ecc_key_pair_new_from_private_key_fn)(struct aws_allocator *allocator,
                                                                            enum aws_ecc_curve_name curve_name,
                                                                            const struct aws_byte_cursor *priv_key);

#ifndef BYO_CRYPTO

extern struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_public_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *public_key_x,
    const struct aws_byte_cursor *public_key_y);

extern struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_private_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *priv_key);

#else /* BYO_CRYPTO */

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_public_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *public_key_x,
    const struct aws_byte_cursor *public_key_y) {
    (void)allocator;
    (void)curve_name;
    (void)public_key_x;
    (void)public_key_y;
    abort();
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_private_key_impl(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *priv_key) {
    (void)allocator;
    (void)curve_name;
    (void)priv_key;
    abort();
}

#endif /* BYO_CRYPTO */

static aws_ecc_key_pair_new_from_public_key_fn *s_ecc_key_pair_new_from_public_key_fn =
    aws_ecc_key_pair_new_from_public_key_impl;

static aws_ecc_key_pair_new_from_private_key_fn *s_ecc_key_pair_new_from_private_key_fn =
    aws_ecc_key_pair_new_from_private_key_impl;

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_public_key(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *public_key_x,
    const struct aws_byte_cursor *public_key_y) {
    return s_ecc_key_pair_new_from_public_key_fn(allocator, curve_name, public_key_x, public_key_y);
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_private_key(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *priv_key) {
    return s_ecc_key_pair_new_from_private_key_fn(allocator, curve_name, priv_key);
}

static void s_aws_ecc_key_pair_destroy(struct aws_ecc_key_pair *key_pair) {
    if (key_pair) {
        AWS_FATAL_ASSERT(key_pair->vtable->destroy && "ECC KEY PAIR destroy function must be included on the vtable");
        key_pair->vtable->destroy(key_pair);
    }
}

int aws_ecc_key_pair_derive_public_key(struct aws_ecc_key_pair *key_pair) {
    AWS_FATAL_ASSERT(key_pair->vtable->derive_pub_key && "ECC KEY PAIR derive function must be included on the vtable");
    return key_pair->vtable->derive_pub_key(key_pair);
}

int aws_ecc_key_pair_sign_message(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *message,
    struct aws_byte_buf *signature) {
    AWS_FATAL_ASSERT(key_pair->vtable->sign_message && "ECC KEY PAIR sign message must be included on the vtable");
    return key_pair->vtable->sign_message(key_pair, message, signature);
}

int aws_ecc_key_pair_verify_signature(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *message,
    const struct aws_byte_cursor *signature) {
    AWS_FATAL_ASSERT(
        key_pair->vtable->verify_signature && "ECC KEY PAIR verify signature must be included on the vtable");
    return key_pair->vtable->verify_signature(key_pair, message, signature);
}

size_t aws_ecc_key_pair_signature_length(const struct aws_ecc_key_pair *key_pair) {
    AWS_FATAL_ASSERT(
        key_pair->vtable->signature_length && "ECC KEY PAIR signature length must be included on the vtable");
    return key_pair->vtable->signature_length(key_pair);
}

void aws_ecc_key_pair_get_public_key(
    const struct aws_ecc_key_pair *key_pair,
    struct aws_byte_cursor *pub_x,
    struct aws_byte_cursor *pub_y) {
    *pub_x = aws_byte_cursor_from_buf(&key_pair->pub_x);
    *pub_y = aws_byte_cursor_from_buf(&key_pair->pub_y);
}

void aws_ecc_key_pair_get_private_key(const struct aws_ecc_key_pair *key_pair, struct aws_byte_cursor *private_d) {
    *private_d = aws_byte_cursor_from_buf(&key_pair->priv_d);
}

size_t aws_ecc_key_coordinate_byte_size_from_curve_name(enum aws_ecc_curve_name curve_name) {
    switch (curve_name) {
        case AWS_CAL_ECDSA_P256:
            return 32;
        case AWS_CAL_ECDSA_P384:
            return 48;
        default:
            return 0;
    }
}

int aws_der_decoder_load_ecc_key_pair(
    struct aws_der_decoder *decoder,
    struct aws_byte_cursor *out_public_x_coor,
    struct aws_byte_cursor *out_public_y_coor,
    struct aws_byte_cursor *out_private_d,
    enum aws_ecc_curve_name *out_curve_name) {

    AWS_ZERO_STRUCT(*out_public_x_coor);
    AWS_ZERO_STRUCT(*out_public_y_coor);
    AWS_ZERO_STRUCT(*out_private_d);

    /* we could have private key or a public key, or a full pair. */
    struct aws_byte_cursor pair_part_1;
    AWS_ZERO_STRUCT(pair_part_1);
    struct aws_byte_cursor pair_part_2;
    AWS_ZERO_STRUCT(pair_part_2);

    bool curve_name_recognized = false;

    /* work with this pointer and move it to the next after using it. We need
     * to know which curve we're dealing with before we can figure out which is which. */
    struct aws_byte_cursor *current_part = &pair_part_1;

    while (aws_der_decoder_next(decoder)) {
        enum aws_der_type type = aws_der_decoder_tlv_type(decoder);

        if (type == AWS_DER_OBJECT_IDENTIFIER) {
            struct aws_byte_cursor oid;
            AWS_ZERO_STRUCT(oid);
            aws_der_decoder_tlv_blob(decoder, &oid);
            /* There can be other OID's so just look for one that is the curve. */
            if (!aws_ecc_curve_name_from_oid(&oid, out_curve_name)) {
                curve_name_recognized = true;
            }
            continue;
        }

        /* you'd think we'd get some type hints on which key this is, but it's not consistent
         * as far as I can tell. */
        if (type == AWS_DER_BIT_STRING || type == AWS_DER_OCTET_STRING) {
            aws_der_decoder_tlv_string(decoder, current_part);
            current_part = &pair_part_2;
        }
    }

    if (!curve_name_recognized) {
        return aws_raise_error(AWS_ERROR_CAL_UNKNOWN_OBJECT_IDENTIFIER);
    }

    size_t key_coordinate_size = aws_ecc_key_coordinate_byte_size_from_curve_name(*out_curve_name);

    struct aws_byte_cursor *private_key = NULL;
    struct aws_byte_cursor *public_key = NULL;

    size_t public_key_blob_size = key_coordinate_size * 2 + 1;

    if (pair_part_1.ptr && pair_part_1.len) {
        if (pair_part_1.len == key_coordinate_size) {
            private_key = &pair_part_1;
        } else if (pair_part_1.len == public_key_blob_size) {
            public_key = &pair_part_1;
        }
    }

    if (pair_part_2.ptr && pair_part_2.len) {
        if (pair_part_2.len == key_coordinate_size) {
            private_key = &pair_part_2;
        } else if (pair_part_2.len == public_key_blob_size) {
            public_key = &pair_part_2;
        }
    }

    if (!private_key && !public_key) {
        return aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
    }

    if (private_key) {
        *out_private_d = *private_key;
    }

    if (public_key) {
        aws_byte_cursor_advance(public_key, 1);
        *out_public_x_coor = *public_key;
        out_public_x_coor->len = key_coordinate_size;
        out_public_y_coor->ptr = public_key->ptr + key_coordinate_size;
        out_public_y_coor->len = key_coordinate_size;
    }

    return AWS_OP_SUCCESS;
}

void aws_ecc_key_pair_acquire(struct aws_ecc_key_pair *key_pair) {
    aws_atomic_fetch_add(&key_pair->ref_count, 1);
}

void aws_ecc_key_pair_release(struct aws_ecc_key_pair *key_pair) {
    if (key_pair == NULL) {
        return;
    }

    size_t old_value = aws_atomic_fetch_sub(&key_pair->ref_count, 1);

    if (old_value == 1) {
        s_aws_ecc_key_pair_destroy(key_pair);
    }
}

struct aws_ecc_key_pair *aws_ecc_key_new_from_hex_coordinates(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    struct aws_byte_cursor pub_x_hex_cursor,
    struct aws_byte_cursor pub_y_hex_cursor) {
    struct aws_byte_buf pub_x_buffer;
    AWS_ZERO_STRUCT(pub_x_buffer);
    struct aws_byte_buf pub_y_buffer;
    AWS_ZERO_STRUCT(pub_y_buffer);

    struct aws_ecc_key_pair *key = NULL;

    size_t pub_x_length = 0;
    size_t pub_y_length = 0;
    if (aws_hex_compute_decoded_len(pub_x_hex_cursor.len, &pub_x_length) ||
        aws_hex_compute_decoded_len(pub_y_hex_cursor.len, &pub_y_length)) {
        goto done;
    }

    if (aws_byte_buf_init(&pub_x_buffer, allocator, pub_x_length) ||
        aws_byte_buf_init(&pub_y_buffer, allocator, pub_y_length)) {
        goto done;
    }

    if (aws_hex_decode(&pub_x_hex_cursor, &pub_x_buffer) || aws_hex_decode(&pub_y_hex_cursor, &pub_y_buffer)) {
        goto done;
    }

    struct aws_byte_cursor pub_x_cursor = aws_byte_cursor_from_buf(&pub_x_buffer);
    struct aws_byte_cursor pub_y_cursor = aws_byte_cursor_from_buf(&pub_y_buffer);

    key = aws_ecc_key_pair_new_from_public_key(allocator, curve_name, &pub_x_cursor, &pub_y_cursor);

done:

    aws_byte_buf_clean_up(&pub_x_buffer);
    aws_byte_buf_clean_up(&pub_y_buffer);

    return key;
}
