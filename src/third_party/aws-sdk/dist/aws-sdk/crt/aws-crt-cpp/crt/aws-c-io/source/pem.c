/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/encoding.h>
#include <aws/common/string.h>
#include <aws/io/pem.h>
#include <aws/io/private/pem_utils.h>

#include <aws/io/logging.h>

enum aws_pem_parse_state {
    BEGIN,
    ON_DATA,
    END,
};

static const struct aws_byte_cursor begin_header = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----BEGIN");
static const struct aws_byte_cursor end_header = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----END");
static const struct aws_byte_cursor dashes = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----");

int aws_sanitize_pem(struct aws_byte_buf *pem, struct aws_allocator *allocator) {
    if (!pem->len) {
        /* reject files with no PEM data */
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    struct aws_byte_buf clean_pem_buf;
    if (aws_byte_buf_init(&clean_pem_buf, allocator, pem->len)) {
        return AWS_OP_ERR;
    }
    struct aws_byte_cursor pem_cursor = aws_byte_cursor_from_buf(pem);
    enum aws_pem_parse_state state = BEGIN;

    for (size_t i = 0; i < pem_cursor.len; i++) {
        /* parse through the pem once */
        char current = *(pem_cursor.ptr + i);
        switch (state) {
            case BEGIN:
                if (current == '-') {
                    struct aws_byte_cursor compare_cursor = pem_cursor;
                    compare_cursor.len = begin_header.len;
                    compare_cursor.ptr += i;
                    if (aws_byte_cursor_eq(&compare_cursor, &begin_header)) {
                        state = ON_DATA;
                        i--;
                    }
                }
                break;
            case ON_DATA:
                /* start copying everything */
                if (current == '-') {
                    struct aws_byte_cursor compare_cursor = pem_cursor;
                    compare_cursor.len = end_header.len;
                    compare_cursor.ptr += i;
                    if (aws_byte_cursor_eq(&compare_cursor, &end_header)) {
                        /* Copy the end header string and start to search for the end part of a pem */
                        state = END;
                        aws_byte_buf_append(&clean_pem_buf, &end_header);
                        i += (end_header.len - 1);
                        break;
                    }
                }
                aws_byte_buf_append_byte_dynamic(&clean_pem_buf, (uint8_t)current);
                break;
            case END:
                if (current == '-') {
                    struct aws_byte_cursor compare_cursor = pem_cursor;
                    compare_cursor.len = dashes.len;
                    compare_cursor.ptr += i;
                    if (aws_byte_cursor_eq(&compare_cursor, &dashes)) {
                        /* End part of a pem, copy the last 5 dashes and a new line, then ignore everything before next
                         * begin header */
                        state = BEGIN;
                        aws_byte_buf_append(&clean_pem_buf, &dashes);
                        i += (dashes.len - 1);
                        aws_byte_buf_append_byte_dynamic(&clean_pem_buf, (uint8_t)'\n');
                        break;
                    }
                }
                aws_byte_buf_append_byte_dynamic(&clean_pem_buf, (uint8_t)current);
                break;
            default:
                break;
        }
    }

    if (clean_pem_buf.len == 0) {
        /* No valid data remains after sanitization. File might have been the wrong format */
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto error;
    }

    struct aws_byte_cursor clean_pem_cursor = aws_byte_cursor_from_buf(&clean_pem_buf);
    aws_byte_buf_reset(pem, true);
    aws_byte_buf_append_dynamic(pem, &clean_pem_cursor);
    aws_byte_buf_clean_up(&clean_pem_buf);
    return AWS_OP_SUCCESS;

error:
    aws_byte_buf_clean_up(&clean_pem_buf);
    return AWS_OP_ERR;
}

/*
 * Possible PEM object types. openssl/pem.h used as a source of truth for
 * possible types.
 */
static struct aws_byte_cursor s_pem_type_x509_old_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("X509 CERTIFICATE");
static struct aws_byte_cursor s_pem_type_x509_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("CERTIFICATE");
static struct aws_byte_cursor s_pem_type_x509_trusted_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("TRUSTED CERTIFICATE");
static struct aws_byte_cursor s_pem_type_x509_req_old_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("NEW CERTIFICATE REQUEST");
static struct aws_byte_cursor s_pem_type_x509_req_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("CERTIFICATE REQUEST");
static struct aws_byte_cursor s_pem_type_x509_crl_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("X509 CRL");
static struct aws_byte_cursor s_pem_type_evp_pkey_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("ANY PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_public_pkcs8_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PUBLIC KEY");
static struct aws_byte_cursor s_pem_type_private_rsa_pkcs1_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("RSA PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_public_rsa_pkcs1_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("RSA PUBLIC KEY");
static struct aws_byte_cursor s_pem_type_private_dsa_pkcs1_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("RSA PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_public_dsa_pkcs1_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("RSA PUBLIC KEY");
static struct aws_byte_cursor s_pem_type_pkcs7_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PKCS7");
static struct aws_byte_cursor s_pem_type_pkcs7_signed_data_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PKCS #7 SIGNED DATA");
static struct aws_byte_cursor s_pem_type_private_pkcs8_encrypted_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("ENCRYPTED PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_private_pkcs8_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_dh_parameters_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("DH PARAMETERS");
static struct aws_byte_cursor s_pem_type_dh_parameters_x942_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("X9.42 DH PARAMETERS");
static struct aws_byte_cursor s_pem_type_ssl_session_parameters_cur =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("SSL SESSION PARAMETERS");
static struct aws_byte_cursor s_pem_type_dsa_parameters_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("DSA PARAMETERS");
static struct aws_byte_cursor s_pem_type_ecdsa_public_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("ECDSA PUBLIC KEY");
static struct aws_byte_cursor s_pem_type_ec_parameters_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("EC PARAMETERS");
static struct aws_byte_cursor s_pem_type_ec_private_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("EC PRIVATE KEY");
static struct aws_byte_cursor s_pem_type_parameters_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PARAMETERS");
static struct aws_byte_cursor s_pem_type_cms_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("CMS");
static struct aws_byte_cursor s_pem_type_sm2_parameters_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("SM2 PARAMETERS");

void aws_pem_objects_clean_up(struct aws_array_list *pem_objects) {
    for (size_t i = 0; i < aws_array_list_length(pem_objects); ++i) {
        struct aws_pem_object *pem_obj_ptr = NULL;
        aws_array_list_get_at_ptr(pem_objects, (void **)&pem_obj_ptr, i);

        if (pem_obj_ptr != NULL) {
            aws_byte_buf_clean_up_secure(&pem_obj_ptr->data);
            aws_string_destroy(pem_obj_ptr->type_string);
        }
    }

    aws_array_list_clear(pem_objects);
    aws_array_list_clean_up(pem_objects);
}

enum aws_pem_object_type s_map_type_cur_to_type(struct aws_byte_cursor type_cur) {
    /*
     * Putting all those in a hash table might be a bit faster depending on
     * hashing function cost, but it complicates code considerably for a
     * potential small gain. PEM parsing is already slow due to multiple
     * allocations and should not be used in perf critical places.
     * So choosing dumb and easy approach over something more complicated and we
     * can reevaluate decision in the future.
     */
    if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_old_cur)) {
        return AWS_PEM_TYPE_X509_OLD;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_cur)) {
        return AWS_PEM_TYPE_X509;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_trusted_cur)) {
        return AWS_PEM_TYPE_X509_TRUSTED;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_req_old_cur)) {
        return AWS_PEM_TYPE_X509_REQ_OLD;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_req_cur)) {
        return AWS_PEM_TYPE_X509_REQ;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_x509_crl_cur)) {
        return AWS_PEM_TYPE_X509_CRL;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_evp_pkey_cur)) {
        return AWS_PEM_TYPE_EVP_PKEY;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_public_pkcs8_cur)) {
        return AWS_PEM_TYPE_PUBLIC_PKCS8;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_private_rsa_pkcs1_cur)) {
        return AWS_PEM_TYPE_PRIVATE_RSA_PKCS1;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_public_rsa_pkcs1_cur)) {
        return AWS_PEM_TYPE_PUBLIC_RSA_PKCS1;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_private_dsa_pkcs1_cur)) {
        return AWS_PEM_TYPE_PRIVATE_DSA_PKCS1;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_public_dsa_pkcs1_cur)) {
        return AWS_PEM_TYPE_PUBLIC_DSA_PKCS1;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_pkcs7_cur)) {
        return AWS_PEM_TYPE_PKCS7;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_pkcs7_signed_data_cur)) {
        return AWS_PEM_TYPE_PKCS7_SIGNED_DATA;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_private_pkcs8_encrypted_cur)) {
        return AWS_PEM_TYPE_PRIVATE_PKCS8_ENCRYPTED;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_private_pkcs8_cur)) {
        return AWS_PEM_TYPE_PRIVATE_PKCS8;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_dh_parameters_cur)) {
        return AWS_PEM_TYPE_DH_PARAMETERS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_dh_parameters_x942_cur)) {
        return AWS_PEM_TYPE_DH_PARAMETERS_X942;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_ssl_session_parameters_cur)) {
        return AWS_PEM_TYPE_SSL_SESSION_PARAMETERS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_dsa_parameters_cur)) {
        return AWS_PEM_TYPE_DSA_PARAMETERS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_ecdsa_public_cur)) {
        return AWS_PEM_TYPE_ECDSA_PUBLIC;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_ec_parameters_cur)) {
        return AWS_PEM_TYPE_EC_PARAMETERS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_ec_private_cur)) {
        return AWS_PEM_TYPE_EC_PRIVATE;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_parameters_cur)) {
        return AWS_PEM_TYPE_PARAMETERS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_cms_cur)) {
        return AWS_PEM_TYPE_CMS;
    } else if (aws_byte_cursor_eq(&type_cur, &s_pem_type_sm2_parameters_cur)) {
        return AWS_PEM_TYPE_SM2_PARAMETERS;
    }

    return AWS_PEM_TYPE_UNKNOWN;
}

static struct aws_byte_cursor s_begin_header_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----BEGIN");
static struct aws_byte_cursor s_end_header_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----END");
static struct aws_byte_cursor s_delim_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("-----");

int s_extract_header_type_cur(struct aws_byte_cursor cur, struct aws_byte_cursor *out) {
    if (!aws_byte_cursor_starts_with(&cur, &s_begin_header_cur)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Invalid PEM buffer: invalid begin token");
        return aws_raise_error(AWS_ERROR_PEM_MALFORMED);
    }

    aws_byte_cursor_advance(&cur, s_begin_header_cur.len);
    aws_byte_cursor_advance(&cur, 1); // space after begin

    struct aws_byte_cursor type_cur = aws_byte_cursor_advance(&cur, cur.len - s_delim_cur.len);

    if (!aws_byte_cursor_eq(&cur, &s_delim_cur)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Invalid PEM buffer: invalid end token");
        return aws_raise_error(AWS_ERROR_PEM_MALFORMED);
    }

    *out = type_cur;
    return AWS_OP_SUCCESS;
}

static int s_convert_pem_to_raw_base64(
    struct aws_allocator *allocator,
    struct aws_byte_cursor pem,
    struct aws_array_list *pem_objects) {

    struct aws_array_list split_buffers;
    if (aws_array_list_init_dynamic(&split_buffers, allocator, 16, sizeof(struct aws_byte_cursor))) {
        return AWS_OP_ERR;
    }

    if (aws_byte_cursor_split_on_char(&pem, '\n', &split_buffers)) {
        aws_array_list_clean_up(&split_buffers);
        AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Invalid PEM buffer: failed to split on newline");
        return aws_raise_error(AWS_ERROR_PEM_MALFORMED);
    }

    enum aws_pem_parse_state state = BEGIN;
    bool on_length_calc = true;
    size_t current_obj_len = 0;
    size_t current_obj_start_index = 0;
    struct aws_byte_buf current_obj_buf;
    AWS_ZERO_STRUCT(current_obj_buf);
    struct aws_byte_cursor current_obj_type_cur;
    AWS_ZERO_STRUCT(current_obj_type_cur);
    enum aws_pem_object_type current_obj_type = AWS_PEM_TYPE_UNKNOWN;

    size_t split_count = aws_array_list_length(&split_buffers);
    size_t i = 0;

    while (i < split_count) {
        struct aws_byte_cursor *line_cur_ptr = NULL;
        int error = aws_array_list_get_at_ptr(&split_buffers, (void **)&line_cur_ptr, i);
        /* should never fail as we control array size and how we index into list */
        AWS_FATAL_ASSERT(error == AWS_OP_SUCCESS);

        /* Burn off the padding in the buffer first.
         * Worst case we'll only have to do this once per line in the buffer. */
        *line_cur_ptr = aws_byte_cursor_left_trim_pred(line_cur_ptr, aws_isspace);

        /* And make sure remove any space from right side */
        *line_cur_ptr = aws_byte_cursor_right_trim_pred(line_cur_ptr, aws_isspace);

        switch (state) {
            case BEGIN:
                if (aws_byte_cursor_starts_with(line_cur_ptr, &s_begin_header_cur)) {
                    if (s_extract_header_type_cur(*line_cur_ptr, &current_obj_type_cur)) {
                        goto on_end_of_loop;
                    }
                    current_obj_type = s_map_type_cur_to_type(current_obj_type_cur);
                    current_obj_start_index = i + 1;
                    state = ON_DATA;
                }
                ++i;
                break;
            /* this loops through the lines containing data twice. First to figure out the length, a second
             * time to actually copy the data. */
            case ON_DATA:
                /* Found end tag. */
                if (aws_byte_cursor_starts_with(line_cur_ptr, &s_end_header_cur)) {
                    if (on_length_calc) {
                        on_length_calc = false;
                        state = ON_DATA;
                        i = current_obj_start_index;
                        aws_byte_buf_init(&current_obj_buf, allocator, current_obj_len);

                    } else {
                        struct aws_pem_object pem_object = {
                            .data = current_obj_buf,
                            .type_string = aws_string_new_from_cursor(allocator, &current_obj_type_cur),
                            .type = current_obj_type,
                        };

                        if (aws_array_list_push_back(pem_objects, &pem_object)) {
                            goto on_end_of_loop;
                        }
                        state = BEGIN;
                        on_length_calc = true;
                        current_obj_len = 0;
                        ++i;
                        AWS_ZERO_STRUCT(current_obj_buf);
                        AWS_ZERO_STRUCT(current_obj_type_cur);
                        current_obj_type = AWS_PEM_TYPE_UNKNOWN;
                    }
                    /* actually on a line with data in it. */
                } else {
                    if (on_length_calc) {
                        current_obj_len += line_cur_ptr->len;
                    } else {
                        if (aws_byte_buf_append(&current_obj_buf, line_cur_ptr)) {
                            goto on_end_of_loop;
                        }
                    }
                    ++i;
                }
                break;
            default:
                AWS_FATAL_ASSERT(false);
        }
    }

/*
 * Note: this function only hard error if nothing can be parsed out of file.
 * Otherwise it succeeds and returns whatever was parsed successfully.
 */
on_end_of_loop:
    aws_array_list_clean_up(&split_buffers);
    aws_byte_buf_clean_up_secure(&current_obj_buf);

    if (state == BEGIN && aws_array_list_length(pem_objects) > 0) {
        return AWS_OP_SUCCESS;
    }

    AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Invalid PEM buffer.");
    aws_pem_objects_clean_up(pem_objects);
    return aws_raise_error(AWS_ERROR_PEM_MALFORMED);
}

int aws_pem_objects_init_from_file_contents(
    struct aws_array_list *pem_objects,
    struct aws_allocator *allocator,
    struct aws_byte_cursor pem_cursor) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(pem_objects != NULL);

    /* Init empty array list, ideally, the PEM should only has one key included. */
    if (aws_array_list_init_dynamic(pem_objects, allocator, 1, sizeof(struct aws_pem_object))) {
        return AWS_OP_ERR;
    }

    if (s_convert_pem_to_raw_base64(allocator, pem_cursor, pem_objects)) {
        goto on_error;
    }

    for (size_t i = 0; i < aws_array_list_length(pem_objects); ++i) {
        struct aws_pem_object *pem_obj_ptr = NULL;
        aws_array_list_get_at_ptr(pem_objects, (void **)&pem_obj_ptr, i);
        struct aws_byte_cursor byte_cur = aws_byte_cursor_from_buf(&pem_obj_ptr->data);

        size_t decoded_len = 0;
        if (aws_base64_compute_decoded_len(&byte_cur, &decoded_len)) {
            AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Failed to get length for decoded base64 pem object.");
            aws_raise_error(AWS_ERROR_PEM_MALFORMED);
            goto on_error;
        }

        struct aws_byte_buf decoded_buffer;
        aws_byte_buf_init(&decoded_buffer, allocator, decoded_len);

        if (aws_base64_decode(&byte_cur, &decoded_buffer)) {
            AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Failed to base 64 decode pem object.");
            aws_raise_error(AWS_ERROR_PEM_MALFORMED);
            aws_byte_buf_clean_up_secure(&decoded_buffer);
            goto on_error;
        }

        aws_byte_buf_clean_up_secure(&pem_obj_ptr->data);
        pem_obj_ptr->data = decoded_buffer;
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_pem_objects_clean_up(pem_objects);
    return AWS_OP_ERR;
}

int aws_pem_objects_init_from_file_path(
    struct aws_array_list *pem_objects,
    struct aws_allocator *allocator,
    const char *filename) {

    struct aws_byte_buf raw_file_buffer;
    if (aws_byte_buf_init_from_file(&raw_file_buffer, allocator, filename)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Failed to read file %s.", filename);
        return AWS_OP_ERR;
    }
    AWS_ASSERT(raw_file_buffer.buffer);

    struct aws_byte_cursor file_cursor = aws_byte_cursor_from_buf(&raw_file_buffer);
    if (aws_pem_objects_init_from_file_contents(pem_objects, allocator, file_cursor)) {
        aws_byte_buf_clean_up_secure(&raw_file_buffer);
        AWS_LOGF_ERROR(AWS_LS_IO_PEM, "Failed to decode PEM file %s.", filename);
        return AWS_OP_ERR;
    }

    aws_byte_buf_clean_up_secure(&raw_file_buffer);

    return AWS_OP_SUCCESS;
}
