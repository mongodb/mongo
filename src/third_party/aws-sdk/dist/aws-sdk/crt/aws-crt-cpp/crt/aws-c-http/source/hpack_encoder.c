/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/hpack.h>

#define HPACK_LOGF(level, encoder, text, ...)                                                                          \
    AWS_LOGF_##level(AWS_LS_HTTP_ENCODER, "id=%p [HPACK]: " text, (encoder)->log_id, __VA_ARGS__)
#define HPACK_LOG(level, encoder, text) HPACK_LOGF(level, encoder, "%s", text)

struct aws_huffman_symbol_coder *hpack_get_coder(void);

void aws_hpack_encoder_init(struct aws_hpack_encoder *encoder, struct aws_allocator *allocator, const void *log_id) {

    AWS_ZERO_STRUCT(*encoder);
    encoder->log_id = log_id;

    aws_huffman_encoder_init(&encoder->huffman_encoder, hpack_get_coder());

    aws_hpack_context_init(&encoder->context, allocator, AWS_LS_HTTP_ENCODER, log_id);

    encoder->dynamic_table_size_update.pending = false;
    encoder->dynamic_table_size_update.latest_value = SIZE_MAX;
    encoder->dynamic_table_size_update.smallest_value = SIZE_MAX;
}

void aws_hpack_encoder_clean_up(struct aws_hpack_encoder *encoder) {
    aws_hpack_context_clean_up(&encoder->context);
    AWS_ZERO_STRUCT(*encoder);
}

void aws_hpack_encoder_set_huffman_mode(struct aws_hpack_encoder *encoder, enum aws_hpack_huffman_mode mode) {
    encoder->huffman_mode = mode;
}

void aws_hpack_encoder_update_max_table_size(struct aws_hpack_encoder *encoder, uint32_t new_max_size) {

    if (!encoder->dynamic_table_size_update.pending) {
        encoder->dynamic_table_size_update.pending = true;
    }
    encoder->dynamic_table_size_update.smallest_value =
        aws_min_size(new_max_size, encoder->dynamic_table_size_update.smallest_value);

    /* TODO: don't necessarily go as high as possible. The peer said the encoder's
     * dynamic table COULD get this big, but it's not required to.
     * It's probably not a good idea to let the peer decide how much memory we allocate.
     * Not sure how to cap it though... Use a hardcoded number?
     * Match whatever SETTINGS_HEADER_TABLE_SIZE this side sends? */
    encoder->dynamic_table_size_update.latest_value = new_max_size;
}

/* Return a byte with the N right-most bits masked.
 * Ex: 2 -> 00000011 */
static uint8_t s_masked_right_bits_u8(uint8_t num_masked_bits) {
    AWS_ASSERT(num_masked_bits <= 8);
    const uint8_t cut_bits = 8 - num_masked_bits;
    return UINT8_MAX >> cut_bits;
}

/* If buffer isn't big enough, grow it intelligently */
static int s_ensure_space(struct aws_byte_buf *output, size_t required_space) {
    size_t available_space = output->capacity - output->len;
    if (required_space <= available_space) {
        return AWS_OP_SUCCESS;
    }

    /* Capacity must grow to at least this size */
    size_t required_capacity;
    if (aws_add_size_checked(output->len, required_space, &required_capacity)) {
        return AWS_OP_ERR;
    }

    /* Prefer to double capacity, but if that's not enough grow to exactly required_capacity */
    size_t double_capacity = aws_add_size_saturating(output->capacity, output->capacity);
    size_t reserve = aws_max_size(required_capacity, double_capacity);
    return aws_byte_buf_reserve(output, reserve);
}

int aws_hpack_encode_integer(
    uint64_t integer,
    uint8_t starting_bits,
    uint8_t prefix_size,
    struct aws_byte_buf *output) {
    AWS_ASSERT(prefix_size <= 8);

    const uint8_t prefix_mask = s_masked_right_bits_u8(prefix_size);
    AWS_ASSERT((starting_bits & prefix_mask) == 0);

    const size_t original_len = output->len;

    if (integer < prefix_mask) {
        /* If the integer fits inside the specified number of bits but won't be all 1's, just write it */

        /* Just write out the bits we care about */
        uint8_t first_byte = starting_bits | (uint8_t)integer;
        if (aws_byte_buf_append_byte_dynamic(output, first_byte)) {
            goto error;
        }
    } else {
        /* Set all of the bits in the first octet to 1 */
        uint8_t first_byte = starting_bits | prefix_mask;
        if (aws_byte_buf_append_byte_dynamic(output, first_byte)) {
            goto error;
        }

        integer -= prefix_mask;

        const uint64_t hi_57bit_mask = UINT64_MAX - (UINT8_MAX >> 1);

        do {
            /* Take top 7 bits from the integer */
            uint8_t this_octet = integer % 128;
            if (integer & hi_57bit_mask) {
                /* If there's more after this octet, set the hi bit */
                this_octet += 128;
            }

            if (aws_byte_buf_append_byte_dynamic(output, this_octet)) {
                goto error;
            }

            /* Remove the written bits */
            integer >>= 7;
        } while (integer);
    }

    return AWS_OP_SUCCESS;
error:
    output->len = original_len;
    return AWS_OP_ERR;
}

int aws_hpack_encode_string(
    struct aws_hpack_encoder *encoder,
    struct aws_byte_cursor to_encode,
    struct aws_byte_buf *output) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&to_encode));
    AWS_PRECONDITION(output);

    const size_t original_len = output->len;

    /* Determine length of encoded string (and whether or not to use huffman) */
    uint8_t use_huffman;
    size_t str_length;
    switch (encoder->huffman_mode) {
        case AWS_HPACK_HUFFMAN_NEVER:
            use_huffman = 0;
            str_length = to_encode.len;
            break;

        case AWS_HPACK_HUFFMAN_ALWAYS:
            use_huffman = 1;
            str_length = aws_huffman_get_encoded_length(&encoder->huffman_encoder, to_encode);
            break;

        case AWS_HPACK_HUFFMAN_SMALLEST:
            str_length = aws_huffman_get_encoded_length(&encoder->huffman_encoder, to_encode);
            if (str_length < to_encode.len) {
                use_huffman = 1;
            } else {
                str_length = to_encode.len;
                use_huffman = 0;
            }
            break;

        default:
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            goto error;
    }

    /*
     * String literals are encoded like so (RFC-7541 5.2):
     * H is whether or not data is huffman-encoded.
     *
     *   0   1   2   3   4   5   6   7
     * +---+---+---+---+---+---+---+---+
     * | H |    String Length (7+)     |
     * +---+---------------------------+
     * |  String Data (Length octets)  |
     * +-------------------------------+
     */

    /* Encode string length */
    uint8_t starting_bits = use_huffman << 7;
    if (aws_hpack_encode_integer(str_length, starting_bits, 7, output)) {
        HPACK_LOGF(ERROR, encoder, "Error encoding HPACK integer: %s", aws_error_name(aws_last_error()));
        goto error;
    }

    /* Encode string data */
    if (str_length > 0) {
        if (use_huffman) {
            /* Huffman encoder doesn't grow buffer, so we ensure it's big enough here */
            if (s_ensure_space(output, str_length)) {
                goto error;
            }

            if (aws_huffman_encode(&encoder->huffman_encoder, &to_encode, output)) {
                HPACK_LOGF(ERROR, encoder, "Error from Huffman encoder: %s", aws_error_name(aws_last_error()));
                goto error;
            }

        } else {
            if (aws_byte_buf_append_dynamic(output, &to_encode)) {
                goto error;
            }
        }
    }

    return AWS_OP_SUCCESS;

error:
    output->len = original_len;
    aws_huffman_encoder_reset(&encoder->huffman_encoder);
    return AWS_OP_ERR;
}

/* All types that HPACK might encode/decode (RFC-7541 6 - Binary Format) */
enum aws_hpack_entry_type {
    AWS_HPACK_ENTRY_INDEXED_HEADER_FIELD,                           /* RFC-7541 6.1 */
    AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITH_INCREMENTAL_INDEXING, /* RFC-7541 6.2.1 */
    AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITHOUT_INDEXING,          /* RFC-7541 6.2.2 */
    AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_NEVER_INDEXED,             /* RFC-7541 6.2.3 */
    AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE,                           /* RFC-7541 6.3 */
    AWS_HPACK_ENTRY_TYPE_COUNT,
};

/**
 * First byte each entry type looks like this (RFC-7541 6):
 * The "xxxxx" part is the "N-bit prefix" of the entry's first encoded integer.
 *
 * 1xxxxxxx: Indexed Header Field Representation
 * 01xxxxxx: Literal Header Field with Incremental Indexing
 * 001xxxxx: Dynamic Table Size Update
 * 0001xxxx: Literal Header Field Never Indexed
 * 0000xxxx: Literal Header Field without Indexing
 */
static const uint8_t s_hpack_entry_starting_bit_pattern[AWS_HPACK_ENTRY_TYPE_COUNT] = {
    [AWS_HPACK_ENTRY_INDEXED_HEADER_FIELD] = 1 << 7,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITH_INCREMENTAL_INDEXING] = 1 << 6,
    [AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE] = 1 << 5,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_NEVER_INDEXED] = 1 << 4,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITHOUT_INDEXING] = 0 << 4,
};

static const uint8_t s_hpack_entry_num_prefix_bits[AWS_HPACK_ENTRY_TYPE_COUNT] = {
    [AWS_HPACK_ENTRY_INDEXED_HEADER_FIELD] = 7,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITH_INCREMENTAL_INDEXING] = 6,
    [AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE] = 5,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_NEVER_INDEXED] = 4,
    [AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITHOUT_INDEXING] = 4,
};

static int s_convert_http_compression_to_literal_entry_type(
    enum aws_http_header_compression compression,
    enum aws_hpack_entry_type *out_entry_type) {

    switch (compression) {
        case AWS_HTTP_HEADER_COMPRESSION_USE_CACHE:
            *out_entry_type = AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITH_INCREMENTAL_INDEXING;
            return AWS_OP_SUCCESS;

        case AWS_HTTP_HEADER_COMPRESSION_NO_CACHE:
            *out_entry_type = AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITHOUT_INDEXING;
            return AWS_OP_SUCCESS;

        case AWS_HTTP_HEADER_COMPRESSION_NO_FORWARD_CACHE:
            *out_entry_type = AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_NEVER_INDEXED;
            return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

static int s_encode_header_field(
    struct aws_hpack_encoder *encoder,
    const struct aws_http_header *header,
    struct aws_byte_buf *output) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(header);
    AWS_PRECONDITION(output);

    size_t original_len = output->len;

    /* Search for header-field in tables */
    bool found_indexed_value;
    size_t header_index = aws_hpack_find_index(&encoder->context, header, true, &found_indexed_value);

    if (header->compression != AWS_HTTP_HEADER_COMPRESSION_USE_CACHE) {
        /* If user doesn't want to use indexed value, then don't use it */
        found_indexed_value = false;
    }

    if (header_index && found_indexed_value) {
        /* Indexed header field */
        const enum aws_hpack_entry_type entry_type = AWS_HPACK_ENTRY_INDEXED_HEADER_FIELD;

        /* encode the one index (along with the entry type), and we're done! */
        uint8_t starting_bit_pattern = s_hpack_entry_starting_bit_pattern[entry_type];
        uint8_t num_prefix_bits = s_hpack_entry_num_prefix_bits[entry_type];
        if (aws_hpack_encode_integer(header_index, starting_bit_pattern, num_prefix_bits, output)) {
            goto error;
        }

        return AWS_OP_SUCCESS;
    }

    /* Else, Literal header field... */

    /* determine exactly which type of literal header-field to encode. */
    enum aws_hpack_entry_type literal_entry_type = AWS_HPACK_ENTRY_TYPE_COUNT;
    if (s_convert_http_compression_to_literal_entry_type(header->compression, &literal_entry_type)) {
        goto error;
    }

    /* the entry type makes up the first few bits of the next integer we encode */
    uint8_t starting_bit_pattern = s_hpack_entry_starting_bit_pattern[literal_entry_type];
    uint8_t num_prefix_bits = s_hpack_entry_num_prefix_bits[literal_entry_type];

    if (header_index) {
        /* Literal header field, indexed name */

        /* first encode the index of name */
        if (aws_hpack_encode_integer(header_index, starting_bit_pattern, num_prefix_bits, output)) {
            goto error;
        }
    } else {
        /* Literal header field, new name */

        /* first encode index of 0 to indicate that header-name is not indexed */
        if (aws_hpack_encode_integer(0, starting_bit_pattern, num_prefix_bits, output)) {
            goto error;
        }

        /* next encode header-name string */
        if (aws_hpack_encode_string(encoder, header->name, output)) {
            goto error;
        }
    }

    /* then encode header-value string, and we're done encoding! */
    if (aws_hpack_encode_string(encoder, header->value, output)) {
        goto error;
    }

    /* if "incremental indexing" type, insert header into the dynamic table. */
    if (AWS_HPACK_ENTRY_LITERAL_HEADER_FIELD_WITH_INCREMENTAL_INDEXING == literal_entry_type) {
        if (aws_hpack_insert_header(&encoder->context, header)) {
            goto error;
        }
    }

    return AWS_OP_SUCCESS;
error:
    output->len = original_len;
    return AWS_OP_ERR;
}

int aws_hpack_encode_header_block(
    struct aws_hpack_encoder *encoder,
    const struct aws_http_headers *headers,
    struct aws_byte_buf *output) {

    /* Encode a dynamic table size update at the beginning of the first header-block
     * following the change to the dynamic table size RFC-7541 4.2 */
    if (encoder->dynamic_table_size_update.pending) {
        if (encoder->dynamic_table_size_update.smallest_value != encoder->dynamic_table_size_update.latest_value) {
            size_t smallest_update_value = encoder->dynamic_table_size_update.smallest_value;
            HPACK_LOGF(
                TRACE, encoder, "Encoding smallest dynamic table size update entry size: %zu", smallest_update_value);
            if (aws_hpack_resize_dynamic_table(&encoder->context, smallest_update_value)) {
                HPACK_LOGF(ERROR, encoder, "Dynamic table resize failed, size: %zu", smallest_update_value);
                return AWS_OP_ERR;
            }
            uint8_t starting_bit_pattern = s_hpack_entry_starting_bit_pattern[AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE];
            uint8_t num_prefix_bits = s_hpack_entry_num_prefix_bits[AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE];
            if (aws_hpack_encode_integer(smallest_update_value, starting_bit_pattern, num_prefix_bits, output)) {
                HPACK_LOGF(
                    ERROR,
                    encoder,
                    "Integer encoding failed for table size update entry, integer: %zu",
                    smallest_update_value);
                return AWS_OP_ERR;
            }
        }
        size_t last_update_value = encoder->dynamic_table_size_update.latest_value;
        HPACK_LOGF(TRACE, encoder, "Encoding last dynamic table size update entry size: %zu", last_update_value);
        if (aws_hpack_resize_dynamic_table(&encoder->context, last_update_value)) {
            HPACK_LOGF(ERROR, encoder, "Dynamic table resize failed, size: %zu", last_update_value);
            return AWS_OP_ERR;
        }
        uint8_t starting_bit_pattern = s_hpack_entry_starting_bit_pattern[AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE];
        uint8_t num_prefix_bits = s_hpack_entry_num_prefix_bits[AWS_HPACK_ENTRY_DYNAMIC_TABLE_RESIZE];
        if (aws_hpack_encode_integer(last_update_value, starting_bit_pattern, num_prefix_bits, output)) {
            HPACK_LOGF(
                ERROR, encoder, "Integer encoding failed for table size update entry, integer: %zu", last_update_value);
            return AWS_OP_ERR;
        }

        encoder->dynamic_table_size_update.pending = false;
        encoder->dynamic_table_size_update.latest_value = SIZE_MAX;
        encoder->dynamic_table_size_update.smallest_value = SIZE_MAX;
    }

    const size_t num_headers = aws_http_headers_count(headers);
    for (size_t i = 0; i < num_headers; ++i) {
        struct aws_http_header header;
        aws_http_headers_get_index(headers, i, &header);
        if (s_encode_header_field(encoder, &header, output)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}
