/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/private/der.h>

#include <aws/cal/cal.h>
#include <aws/common/byte_buf.h>

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4204 4221) /* non-standard aggregate initializer warnings */
#endif

struct aws_der_encoder {
    struct aws_allocator *allocator;
    struct aws_byte_buf storage;
    struct aws_byte_buf *buffer; /* buffer being written to, might be storage, might be a sequence/set buffer */
    struct aws_array_list stack;
};

struct aws_der_decoder {
    struct aws_allocator *allocator;
    struct aws_array_list tlvs;   /* parsed elements */
    int tlv_idx;                  /* index to elements after parsing */
    struct aws_byte_cursor input; /* input buffer */
    uint32_t depth;               /* recursion depth when expanding containers */
    struct der_tlv *container;    /* currently expanding container */
};

struct der_tlv {
    uint8_t tag;
    uint32_t length; /* length of value in bytes */
    uint32_t count;  /* SEQUENCE or SET element count */
    uint8_t *value;
};

static int s_decode_tlv(struct der_tlv *tlv) {
    if (tlv->tag == AWS_DER_INTEGER) {
        if (tlv->length == 0) {
            return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
        }

        uint8_t first_byte = tlv->value[0];
        if (first_byte & 0x80) {
            return aws_raise_error(AWS_ERROR_CAL_DER_UNSUPPORTED_NEGATIVE_INT);
        }

        /* if its multibyte int and first byte is 0, strip it since it was added
         * to indicate to der that it is positive number.
         * if len is 1 and first byte is 0, then the number is just zero, so
         * leave it as is.
         */
        if (tlv->length > 1 && first_byte == 0x00) {
            tlv->length -= 1;
            tlv->value += 1;
        }
    } else if (tlv->tag == AWS_DER_BIT_STRING) {
        /* skip over the trailing skipped bit count */
        tlv->length -= 1;
        tlv->value += 1;
    }

    return AWS_OP_SUCCESS;
}

static int s_der_read_tlv(struct aws_byte_cursor *cur, struct der_tlv *tlv) {
    uint8_t tag = 0;
    uint8_t len_bytes = 0;
    uint32_t len = 0;
    if (!aws_byte_cursor_read_u8(cur, &tag)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }
    if (!aws_byte_cursor_read_u8(cur, &len_bytes)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }
    /* if the sign bit is set, then the first byte is the number of bytes required to store
     * the length */
    if (len_bytes & 0x80) {
        len_bytes &= 0x7f;
        switch (len_bytes) {
            case 1:
                if (!aws_byte_cursor_read_u8(cur, (uint8_t *)&len)) {
                    return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
                }
                break;
            case 2:
                if (!aws_byte_cursor_read_be16(cur, (uint16_t *)&len)) {
                    return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
                }
                break;
            case 4:
                if (!aws_byte_cursor_read_be32(cur, &len)) {
                    return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
                }
                break;
            default:
                return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
        }
    } else {
        len = len_bytes;
    }

    if (len > cur->len) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    tlv->tag = tag;
    tlv->length = len;
    tlv->value = (tag == AWS_DER_NULL) ? NULL : cur->ptr;
    if (s_decode_tlv(tlv)) {
        return AWS_OP_ERR;
    }
    aws_byte_cursor_advance(cur, len);

    return AWS_OP_SUCCESS;
}

static uint32_t s_encoded_len(struct der_tlv *tlv) {
    if (tlv->tag == AWS_DER_INTEGER) {
        uint8_t first_byte = tlv->value[0];
        /* if the first byte has the high bit set, a 0 will be prepended to denote unsigned */
        return tlv->length + ((first_byte & 0x80) != 0);
    }
    if (tlv->tag == AWS_DER_BIT_STRING) {
        return tlv->length + 1; /* needs a byte to denote how many trailing skipped bits */
    }

    return tlv->length;
}

static int s_der_write_tlv(struct der_tlv *tlv, struct aws_byte_buf *buf) {
    if (!aws_byte_buf_write_u8(buf, tlv->tag)) {
        return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
    }
    uint32_t len = s_encoded_len(tlv);
    if (len > UINT16_MAX) {
        /* write the high bit plus 4 byte length */
        if (!aws_byte_buf_write_u8(buf, 0x84)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
        if (!aws_byte_buf_write_be32(buf, len)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
    } else if (len > UINT8_MAX) {
        /* write the high bit plus 2 byte length */
        if (!aws_byte_buf_write_u8(buf, 0x82)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
        if (!aws_byte_buf_write_be16(buf, (uint16_t)len)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
    } else if (len > INT8_MAX) {
        /* Write the high bit + 1 byte length */
        if (!aws_byte_buf_write_u8(buf, 0x81)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
        if (!aws_byte_buf_write_u8(buf, (uint8_t)len)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
    } else {
        if (!aws_byte_buf_write_u8(buf, (uint8_t)len)) {
            return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
        }
    }

    switch (tlv->tag) {
        case AWS_DER_INTEGER: {
            /* if the first byte has the sign bit set, insert an extra 0x00 byte to indicate unsigned */
            uint8_t first_byte = tlv->value[0];
            if (first_byte & 0x80) {
                if (!aws_byte_buf_write_u8(buf, 0)) {
                    return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
                }
            }
            if (!aws_byte_buf_write(buf, tlv->value, tlv->length)) {
                return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
            }
        } break;
        case AWS_DER_BOOLEAN:
            if (!aws_byte_buf_write_u8(buf, (*tlv->value) ? 0xff : 0x00)) {
                return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
            }
            break;
        case AWS_DER_BIT_STRING:
            /* Write that there are 0 skipped bits */
            if (!aws_byte_buf_write_u8(buf, 0)) {
                return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
            }
            /* FALLTHROUGH */
        case AWS_DER_BMPString:
        case AWS_DER_IA5String:
        case AWS_DER_PrintableString:
        case AWS_DER_UTF8_STRING:
        case AWS_DER_OBJECT_IDENTIFIER:
        case AWS_DER_OCTET_STRING:
        case AWS_DER_SEQUENCE:
        case AWS_DER_SET:
            if (!aws_byte_buf_write(buf, tlv->value, tlv->length)) {
                return aws_raise_error(AWS_ERROR_INVALID_BUFFER_SIZE);
            }
            break;
        case AWS_DER_NULL:
            /* No value bytes */
            break;
        default:
            return aws_raise_error(AWS_ERROR_CAL_MISMATCHED_DER_TYPE);
    }

    return AWS_OP_SUCCESS;
}

struct aws_der_encoder *aws_der_encoder_new(struct aws_allocator *allocator, size_t capacity) {
    struct aws_der_encoder *encoder = aws_mem_calloc(allocator, 1, sizeof(struct aws_der_encoder));
    AWS_FATAL_ASSERT(encoder);

    encoder->allocator = allocator;
    if (aws_byte_buf_init(&encoder->storage, encoder->allocator, capacity)) {
        goto error;
    }
    if (aws_array_list_init_dynamic(&encoder->stack, encoder->allocator, 4, sizeof(struct der_tlv))) {
        goto error;
    }

    encoder->buffer = &encoder->storage;
    return encoder;

error:
    aws_array_list_clean_up(&encoder->stack);
    aws_byte_buf_clean_up(&encoder->storage);
    aws_mem_release(allocator, encoder);
    return NULL;
}

void aws_der_encoder_destroy(struct aws_der_encoder *encoder) {
    if (!encoder) {
        return;
    }
    aws_byte_buf_clean_up_secure(&encoder->storage);
    aws_array_list_clean_up(&encoder->stack);
    aws_mem_release(encoder->allocator, encoder);
}

int aws_der_encoder_write_unsigned_integer(struct aws_der_encoder *encoder, struct aws_byte_cursor integer) {
    AWS_FATAL_ASSERT(integer.len <= UINT32_MAX);
    struct der_tlv tlv = {
        .tag = AWS_DER_INTEGER,
        .length = (uint32_t)integer.len,
        .value = integer.ptr,
    };

    return s_der_write_tlv(&tlv, encoder->buffer);
}

int aws_der_encoder_write_boolean(struct aws_der_encoder *encoder, bool boolean) {
    struct der_tlv tlv = {.tag = AWS_DER_BOOLEAN, .length = 1, .value = (uint8_t *)&boolean};

    return s_der_write_tlv(&tlv, encoder->buffer);
}

int aws_der_encoder_write_null(struct aws_der_encoder *encoder) {
    struct der_tlv tlv = {
        .tag = AWS_DER_NULL,
        .length = 0,
        .value = NULL,
    };

    return s_der_write_tlv(&tlv, encoder->buffer);
}

int aws_der_encoder_write_bit_string(struct aws_der_encoder *encoder, struct aws_byte_cursor bit_string) {
    AWS_FATAL_ASSERT(bit_string.len <= UINT32_MAX);
    struct der_tlv tlv = {
        .tag = AWS_DER_BIT_STRING,
        .length = (uint32_t)bit_string.len,
        .value = bit_string.ptr,
    };

    return s_der_write_tlv(&tlv, encoder->buffer);
}

int aws_der_encoder_write_octet_string(struct aws_der_encoder *encoder, struct aws_byte_cursor octet_string) {
    AWS_FATAL_ASSERT(octet_string.len <= UINT32_MAX);
    struct der_tlv tlv = {
        .tag = AWS_DER_OCTET_STRING,
        .length = (uint32_t)octet_string.len,
        .value = octet_string.ptr,
    };

    return s_der_write_tlv(&tlv, encoder->buffer);
}

static int s_der_encoder_begin_container(struct aws_der_encoder *encoder, enum aws_der_type type) {
    struct aws_byte_buf *seq_buf = aws_mem_acquire(encoder->allocator, sizeof(struct aws_byte_buf));
    AWS_FATAL_ASSERT(seq_buf);
    if (aws_byte_buf_init(seq_buf, encoder->allocator, encoder->storage.capacity)) {
        return AWS_OP_ERR;
    }
    struct der_tlv tlv_seq = {
        .tag = type,
        .length = 0, /* not known yet, will update later */
        .value = (void *)seq_buf,
    };
    if (aws_array_list_push_back(&encoder->stack, &tlv_seq)) {
        aws_byte_buf_clean_up(seq_buf);
        return AWS_OP_ERR;
    }
    encoder->buffer = seq_buf;
    return AWS_OP_SUCCESS;
}

static int s_der_encoder_end_container(struct aws_der_encoder *encoder) {
    struct der_tlv tlv;
    if (aws_array_list_back(&encoder->stack, &tlv)) {
        return AWS_OP_ERR;
    }
    aws_array_list_pop_back(&encoder->stack);
    /* update the buffer to point at the next container on the stack */
    if (encoder->stack.length > 0) {
        struct der_tlv outer;
        if (aws_array_list_back(&encoder->stack, &outer)) {
            return AWS_OP_ERR;
        }
        encoder->buffer = (struct aws_byte_buf *)outer.value;
    } else {
        encoder->buffer = &encoder->storage;
    }

    struct aws_byte_buf *seq_buf = (struct aws_byte_buf *)tlv.value;
    tlv.length = (uint32_t)seq_buf->len;
    tlv.value = seq_buf->buffer;
    int result = s_der_write_tlv(&tlv, encoder->buffer);
    aws_byte_buf_clean_up_secure(seq_buf);
    aws_mem_release(encoder->allocator, seq_buf);
    return result;
}

int aws_der_encoder_begin_sequence(struct aws_der_encoder *encoder) {
    return s_der_encoder_begin_container(encoder, AWS_DER_SEQUENCE);
}

int aws_der_encoder_end_sequence(struct aws_der_encoder *encoder) {
    return s_der_encoder_end_container(encoder);
}

int aws_der_encoder_begin_set(struct aws_der_encoder *encoder) {
    return s_der_encoder_begin_container(encoder, AWS_DER_SET);
}

int aws_der_encoder_end_set(struct aws_der_encoder *encoder) {
    return s_der_encoder_end_container(encoder);
}

int aws_der_encoder_get_contents(struct aws_der_encoder *encoder, struct aws_byte_cursor *contents) {
    if (encoder->storage.len == 0) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    if (encoder->buffer != &encoder->storage) {
        /* someone forgot to end a sequence or set */
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    *contents = aws_byte_cursor_from_buf(&encoder->storage);
    return AWS_OP_SUCCESS;
}

/*
 * DECODER
 */
int s_decoder_parse(struct aws_der_decoder *decoder);

struct aws_der_decoder *aws_der_decoder_new(struct aws_allocator *allocator, struct aws_byte_cursor input) {
    struct aws_der_decoder *decoder = aws_mem_calloc(allocator, 1, sizeof(struct aws_der_decoder));
    AWS_FATAL_ASSERT(decoder);

    decoder->allocator = allocator;
    decoder->input = input;
    decoder->tlv_idx = -1;
    decoder->depth = 0;
    decoder->container = NULL;
    if (aws_array_list_init_dynamic(&decoder->tlvs, decoder->allocator, 16, sizeof(struct der_tlv))) {
        goto error;
    }

    if (s_decoder_parse(decoder)) {
        goto error;
    }

    return decoder;

error:
    aws_array_list_clean_up(&decoder->tlvs);
    aws_mem_release(allocator, decoder);
    return NULL;
}

void aws_der_decoder_destroy(struct aws_der_decoder *decoder) {
    if (!decoder) {
        return;
    }
    aws_array_list_clean_up(&decoder->tlvs);
    aws_mem_release(decoder->allocator, decoder);
}

int s_parse_cursor(struct aws_der_decoder *decoder, struct aws_byte_cursor cur) {
    if (++decoder->depth > 16) {
        /* stream contains too many nested containers, probably malformed/attack */
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    while (cur.len) {
        struct der_tlv tlv = {0};
        if (s_der_read_tlv(&cur, &tlv)) {
            return AWS_OP_ERR;
        }
        /* skip trailing newlines in the stream after any TLV */
        while (cur.len && *cur.ptr == '\n') {
            aws_byte_cursor_advance(&cur, 1);
        }

        if (aws_array_list_push_back(&decoder->tlvs, &tlv)) {
            return aws_raise_error(AWS_ERROR_INVALID_STATE);
        }
        if (decoder->container) {
            decoder->container->count++;
        }
        /* if the last element was a container, expand it recursively to maintain order */
        if (tlv.tag & AWS_DER_FORM_CONSTRUCTED) {
            struct der_tlv *outer_container = decoder->container;
            struct der_tlv *container = NULL;
            aws_array_list_get_at_ptr(&decoder->tlvs, (void **)&container, decoder->tlvs.length - 1);
            decoder->container = container;

            if (!container) {
                return aws_raise_error(AWS_ERROR_INVALID_STATE);
            }

            struct aws_byte_cursor container_cur = aws_byte_cursor_from_array(container->value, container->length);
            if (s_parse_cursor(decoder, container_cur)) {
                return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
            }
            decoder->container = outer_container; /* restore the container stack */
        }
    }

    --decoder->depth;

    return AWS_OP_SUCCESS;
}

int s_decoder_parse(struct aws_der_decoder *decoder) {
    return s_parse_cursor(decoder, decoder->input);
}

bool aws_der_decoder_next(struct aws_der_decoder *decoder) {
    return (++decoder->tlv_idx < (int)decoder->tlvs.length);
}

static struct der_tlv s_decoder_tlv(struct aws_der_decoder *decoder) {
    AWS_FATAL_ASSERT(decoder->tlv_idx < (int)decoder->tlvs.length);
    struct der_tlv tlv = {0};
    aws_array_list_get_at(&decoder->tlvs, &tlv, decoder->tlv_idx);
    return tlv;
}

enum aws_der_type aws_der_decoder_tlv_type(struct aws_der_decoder *decoder) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    return tlv.tag;
}

size_t aws_der_decoder_tlv_length(struct aws_der_decoder *decoder) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    return tlv.length;
}

size_t aws_der_decoder_tlv_count(struct aws_der_decoder *decoder) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    AWS_FATAL_ASSERT(tlv.tag & AWS_DER_FORM_CONSTRUCTED);
    return tlv.count;
}

static void s_tlv_to_blob(struct der_tlv *tlv, struct aws_byte_cursor *blob) {
    AWS_FATAL_ASSERT(tlv->tag != AWS_DER_NULL);
    *blob = aws_byte_cursor_from_array(tlv->value, tlv->length);
}

int aws_der_decoder_tlv_string(struct aws_der_decoder *decoder, struct aws_byte_cursor *string) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    if (tlv.tag != AWS_DER_OCTET_STRING && tlv.tag != AWS_DER_BIT_STRING) {
        return aws_raise_error(AWS_ERROR_CAL_MISMATCHED_DER_TYPE);
    }
    s_tlv_to_blob(&tlv, string);
    return AWS_OP_SUCCESS;
}

int aws_der_decoder_tlv_unsigned_integer(struct aws_der_decoder *decoder, struct aws_byte_cursor *integer) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    if (tlv.tag != AWS_DER_INTEGER) {
        return aws_raise_error(AWS_ERROR_CAL_MISMATCHED_DER_TYPE);
    }
    s_tlv_to_blob(&tlv, integer);
    return AWS_OP_SUCCESS;
}

int aws_der_decoder_tlv_boolean(struct aws_der_decoder *decoder, bool *boolean) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    if (tlv.tag != AWS_DER_BOOLEAN) {
        return aws_raise_error(AWS_ERROR_CAL_MISMATCHED_DER_TYPE);
    }
    *boolean = *tlv.value != 0;
    return AWS_OP_SUCCESS;
}

int aws_der_decoder_tlv_blob(struct aws_der_decoder *decoder, struct aws_byte_cursor *blob) {
    struct der_tlv tlv = s_decoder_tlv(decoder);
    s_tlv_to_blob(&tlv, blob);
    return AWS_OP_SUCCESS;
}

#ifdef _MSC_VER
#    pragma warning(pop)
#endif
