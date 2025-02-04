/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/hpack.h>

#define HPACK_LOGF(level, decoder, text, ...)                                                                          \
    AWS_LOGF_##level(AWS_LS_HTTP_DECODER, "id=%p [HPACK]: " text, (decoder)->log_id, __VA_ARGS__)
#define HPACK_LOG(level, decoder, text) HPACK_LOGF(level, decoder, "%s", text)

struct aws_huffman_symbol_coder *hpack_get_coder(void);

/* Used while decoding the header name & value, grows if necessary */
const size_t s_hpack_decoder_scratch_initial_size = 512;

void aws_hpack_decoder_init(struct aws_hpack_decoder *decoder, struct aws_allocator *allocator, const void *log_id) {
    AWS_ZERO_STRUCT(*decoder);
    decoder->log_id = log_id;

    aws_huffman_decoder_init(&decoder->huffman_decoder, hpack_get_coder());
    aws_huffman_decoder_allow_growth(&decoder->huffman_decoder, true);

    aws_hpack_context_init(&decoder->context, allocator, AWS_LS_HTTP_DECODER, log_id);

    aws_byte_buf_init(&decoder->progress_entry.scratch, allocator, s_hpack_decoder_scratch_initial_size);

    decoder->dynamic_table_protocol_max_size_setting = aws_hpack_get_dynamic_table_max_size(&decoder->context);
}

void aws_hpack_decoder_clean_up(struct aws_hpack_decoder *decoder) {
    aws_hpack_context_clean_up(&decoder->context);
    aws_byte_buf_clean_up(&decoder->progress_entry.scratch);
    AWS_ZERO_STRUCT(*decoder);
}

static const struct aws_http_header *s_get_header_u64(const struct aws_hpack_decoder *decoder, uint64_t index) {
    if (index > SIZE_MAX) {
        HPACK_LOG(ERROR, decoder, "Header index is absurdly large");
        aws_raise_error(AWS_ERROR_INVALID_INDEX);
        return NULL;
    }

    return aws_hpack_get_header(&decoder->context, (size_t)index);
}

void aws_hpack_decoder_update_max_table_size(struct aws_hpack_decoder *decoder, uint32_t setting_max_size) {
    decoder->dynamic_table_protocol_max_size_setting = setting_max_size;
}

/* Return a byte with the N right-most bits masked.
 * Ex: 2 -> 00000011 */
static uint8_t s_masked_right_bits_u8(uint8_t num_masked_bits) {
    AWS_ASSERT(num_masked_bits <= 8);
    const uint8_t cut_bits = 8 - num_masked_bits;
    return UINT8_MAX >> cut_bits;
}

int aws_hpack_decode_integer(
    struct aws_hpack_decoder *decoder,
    struct aws_byte_cursor *to_decode,
    uint8_t prefix_size,
    uint64_t *integer,
    bool *complete) {

    AWS_PRECONDITION(decoder);
    AWS_PRECONDITION(to_decode);
    AWS_PRECONDITION(prefix_size <= 8);
    AWS_PRECONDITION(integer);

    const uint8_t prefix_mask = s_masked_right_bits_u8(prefix_size);

    struct hpack_progress_integer *progress = &decoder->progress_integer;

    while (to_decode->len) {
        switch (progress->state) {
            case HPACK_INTEGER_STATE_INIT: {
                /* Read the first byte, and check whether this is it, or we need to continue */
                uint8_t byte = 0;
                bool succ = aws_byte_cursor_read_u8(to_decode, &byte);
                AWS_FATAL_ASSERT(succ);

                /* Cut the prefix */
                byte &= prefix_mask;

                /* No matter what, the first byte's value is always added to the integer */
                *integer = byte;

                if (byte != prefix_mask) {
                    goto handle_complete;
                }

                progress->state = HPACK_INTEGER_STATE_VALUE;
            } break;

            case HPACK_INTEGER_STATE_VALUE: {
                uint8_t byte = 0;
                bool succ = aws_byte_cursor_read_u8(to_decode, &byte);
                AWS_FATAL_ASSERT(succ);

                uint64_t new_byte_value = (uint64_t)(byte & 127) << progress->bit_count;
                if (*integer + new_byte_value < *integer) {
                    return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
                }
                *integer += new_byte_value;

                /* Check if we're done */
                if ((byte & 128) == 0) {
                    goto handle_complete;
                }

                /* Increment the bit count */
                progress->bit_count += 7;

                /* 7 Bits are expected to be used, so if we get to the point where any of
                 * those bits can't be used it's a decoding error */
                if (progress->bit_count > 64 - 7) {
                    return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
                }
            } break;
        }
    }

    /* Fell out of data loop, must need more data */
    *complete = false;
    return AWS_OP_SUCCESS;

handle_complete:
    AWS_ZERO_STRUCT(decoder->progress_integer);
    *complete = true;
    return AWS_OP_SUCCESS;
}

int aws_hpack_decode_string(
    struct aws_hpack_decoder *decoder,
    struct aws_byte_cursor *to_decode,
    struct aws_byte_buf *output,
    bool *complete) {

    AWS_PRECONDITION(decoder);
    AWS_PRECONDITION(to_decode);
    AWS_PRECONDITION(output);
    AWS_PRECONDITION(complete);

    struct hpack_progress_string *progress = &decoder->progress_string;

    while (to_decode->len) {
        switch (progress->state) {
            case HPACK_STRING_STATE_INIT: {
                /* Do init stuff */
                progress->state = HPACK_STRING_STATE_LENGTH;
                progress->use_huffman = *to_decode->ptr >> 7;
                aws_huffman_decoder_reset(&decoder->huffman_decoder);
                /* fallthrough, since we didn't consume any data */
            }
            /* FALLTHRU */
            case HPACK_STRING_STATE_LENGTH: {
                bool length_complete = false;
                if (aws_hpack_decode_integer(decoder, to_decode, 7, &progress->length, &length_complete)) {
                    return AWS_OP_ERR;
                }

                if (!length_complete) {
                    goto handle_ongoing;
                }

                if (progress->length == 0) {
                    goto handle_complete;
                }

                if (progress->length > SIZE_MAX) {
                    return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
                }

                progress->state = HPACK_STRING_STATE_VALUE;
            } break;

            case HPACK_STRING_STATE_VALUE: {
                /* Take either as much data as we need, or as much as we can */
                size_t to_process = aws_min_size((size_t)progress->length, to_decode->len);
                progress->length -= to_process;

                struct aws_byte_cursor chunk = aws_byte_cursor_advance(to_decode, to_process);

                if (progress->use_huffman) {
                    if (aws_huffman_decode(&decoder->huffman_decoder, &chunk, output)) {
                        HPACK_LOGF(ERROR, decoder, "Error from Huffman decoder: %s", aws_error_name(aws_last_error()));
                        return AWS_OP_ERR;
                    }

                    /* Decoder should consume all bytes we feed it.
                     * EOS (end-of-string) symbol could stop it early, but HPACK says to treat EOS as error. */
                    if (chunk.len != 0) {
                        HPACK_LOG(ERROR, decoder, "Huffman encoded end-of-string symbol is illegal");
                        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    }
                } else {
                    if (aws_byte_buf_append_dynamic(output, &chunk)) {
                        return AWS_OP_ERR;
                    }
                }

                /* If whole length consumed, we're done */
                if (progress->length == 0) {
                    /* #TODO Validate any padding bits left over in final byte of string.
                     * "A padding not corresponding to the most significant bits of the
                     * code for the EOS symbol MUST be treated as a decoding error" */

                    /* #TODO impose limits on string length */

                    goto handle_complete;
                }
            } break;
        }
    }

handle_ongoing:
    /* Fell out of to_decode loop, must still be in progress */
    AWS_ASSERT(to_decode->len == 0);
    *complete = false;
    return AWS_OP_SUCCESS;

handle_complete:
    AWS_ASSERT(decoder->progress_string.length == 0);
    AWS_ZERO_STRUCT(decoder->progress_string);
    *complete = true;
    return AWS_OP_SUCCESS;
}

/* Implements RFC-7541 Section 6 - Binary Format */
int aws_hpack_decode(
    struct aws_hpack_decoder *decoder,
    struct aws_byte_cursor *to_decode,
    struct aws_hpack_decode_result *result) {

    AWS_PRECONDITION(decoder);
    AWS_PRECONDITION(to_decode);
    AWS_PRECONDITION(result);

    /* Run state machine until we decode a complete entry.
     * Every state requires data, so we can simply loop until no more data available. */
    while (to_decode->len) {
        switch (decoder->progress_entry.state) {

            case HPACK_ENTRY_STATE_INIT: {
                /* Reset entry */
                AWS_ZERO_STRUCT(decoder->progress_entry.u);
                decoder->progress_entry.scratch.len = 0;

                /* Determine next state by looking at first few bits of the next byte:
                 * 1xxxxxxx: Indexed Header Field Representation
                 * 01xxxxxx: Literal Header Field with Incremental Indexing
                 * 001xxxxx: Dynamic Table Size Update
                 * 0001xxxx: Literal Header Field Never Indexed
                 * 0000xxxx: Literal Header Field without Indexing */
                uint8_t first_byte = to_decode->ptr[0];
                if (first_byte & (1 << 7)) {
                    /* 1xxxxxxx: Indexed Header Field Representation */
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_INDEXED;

                } else if (first_byte & (1 << 6)) {
                    /* 01xxxxxx: Literal Header Field with Incremental Indexing */
                    decoder->progress_entry.u.literal.compression = AWS_HTTP_HEADER_COMPRESSION_USE_CACHE;
                    decoder->progress_entry.u.literal.prefix_size = 6;
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_BEGIN;

                } else if (first_byte & (1 << 5)) {
                    /* 001xxxxx: Dynamic Table Size Update */
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_DYNAMIC_TABLE_RESIZE;

                } else if (first_byte & (1 << 4)) {
                    /* 0001xxxx: Literal Header Field Never Indexed */
                    decoder->progress_entry.u.literal.compression = AWS_HTTP_HEADER_COMPRESSION_NO_FORWARD_CACHE;
                    decoder->progress_entry.u.literal.prefix_size = 4;
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_BEGIN;
                } else {
                    /* 0000xxxx: Literal Header Field without Indexing */
                    decoder->progress_entry.u.literal.compression = AWS_HTTP_HEADER_COMPRESSION_NO_CACHE;
                    decoder->progress_entry.u.literal.prefix_size = 4;
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_BEGIN;
                }
            } break;

            /* RFC-7541 6.1. Indexed Header Field Representation.
             * Decode one integer, which is an index into the table.
             * Result is the header name and value stored there. */
            case HPACK_ENTRY_STATE_INDEXED: {
                bool complete = false;
                uint64_t *index = &decoder->progress_entry.u.indexed.index;
                if (aws_hpack_decode_integer(decoder, to_decode, 7, index, &complete)) {
                    return AWS_OP_ERR;
                }

                if (!complete) {
                    break;
                }

                const struct aws_http_header *header = s_get_header_u64(decoder, *index);
                if (!header) {
                    return AWS_OP_ERR;
                }

                result->type = AWS_HPACK_DECODE_T_HEADER_FIELD;
                result->data.header_field = *header;
                goto handle_complete;
            } break;

            /* RFC-7541 6.2. Literal Header Field Representation.
             * We use multiple states to decode a literal...
             * The header-name MAY come from the table and MAY be encoded as a string.
             * The header-value is ALWAYS encoded as a string.
             *
             * This BEGIN state decodes one integer.
             * If it's non-zero, then it's the index in the table where we'll get the header-name from.
             * If it's zero, then we move to the HEADER_NAME state and decode header-name as a string instead */
            case HPACK_ENTRY_STATE_LITERAL_BEGIN: {
                struct hpack_progress_literal *literal = &decoder->progress_entry.u.literal;

                bool index_complete = false;
                if (aws_hpack_decode_integer(
                        decoder, to_decode, literal->prefix_size, &literal->name_index, &index_complete)) {
                    return AWS_OP_ERR;
                }

                if (!index_complete) {
                    break;
                }

                if (literal->name_index == 0) {
                    /* Index 0 means header-name is not in table. Need to decode header-name as a string instead */
                    decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_NAME_STRING;
                    break;
                }

                /* Otherwise we found index of header-name in table. */
                const struct aws_http_header *header = s_get_header_u64(decoder, literal->name_index);
                if (!header) {
                    return AWS_OP_ERR;
                }

                /* Store the name in scratch. We don't just keep a pointer to it because it could be
                 * evicted from the dynamic table later, when we save the literal. */
                if (aws_byte_buf_append_dynamic(&decoder->progress_entry.scratch, &header->name)) {
                    return AWS_OP_ERR;
                }

                /* Move on to decoding header-value.
                 * Value will also decode into the scratch, so save where name ends. */
                literal->name_length = header->name.len;
                decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_VALUE_STRING;
            } break;

            /* We only end up in this state if header-name is encoded as string. */
            case HPACK_ENTRY_STATE_LITERAL_NAME_STRING: {
                bool string_complete = false;
                if (aws_hpack_decode_string(decoder, to_decode, &decoder->progress_entry.scratch, &string_complete)) {
                    return AWS_OP_ERR;
                }

                if (!string_complete) {
                    break;
                }

                /* Done decoding name string! Move on to decoding the value string.
                 * Value will also decode into the scratch, so save where name ends. */
                decoder->progress_entry.u.literal.name_length = decoder->progress_entry.scratch.len;
                decoder->progress_entry.state = HPACK_ENTRY_STATE_LITERAL_VALUE_STRING;
            } break;

            /* Final state for "literal" entries.
             * Decode the header-value string, then deliver the results. */
            case HPACK_ENTRY_STATE_LITERAL_VALUE_STRING: {
                bool string_complete = false;
                if (aws_hpack_decode_string(decoder, to_decode, &decoder->progress_entry.scratch, &string_complete)) {
                    return AWS_OP_ERR;
                }

                if (!string_complete) {
                    break;
                }

                /* Done decoding value string. Done decoding entry. */
                struct hpack_progress_literal *literal = &decoder->progress_entry.u.literal;

                /* Set up a header with name and value (which are packed one after the other in scratch) */
                struct aws_http_header header;
                header.value = aws_byte_cursor_from_buf(&decoder->progress_entry.scratch);
                header.name = aws_byte_cursor_advance(&header.value, literal->name_length);
                header.compression = literal->compression;

                /* Save to table if necessary */
                if (literal->compression == AWS_HTTP_HEADER_COMPRESSION_USE_CACHE) {
                    if (aws_hpack_insert_header(&decoder->context, &header)) {
                        return AWS_OP_ERR;
                    }
                }

                result->type = AWS_HPACK_DECODE_T_HEADER_FIELD;
                result->data.header_field = header;
                goto handle_complete;
            } break;

            /* RFC-7541 6.3. Dynamic Table Size Update
             * Read one integer, which is the new maximum size for the dynamic table. */
            case HPACK_ENTRY_STATE_DYNAMIC_TABLE_RESIZE: {
                uint64_t *size64 = &decoder->progress_entry.u.dynamic_table_resize.size;
                bool size_complete = false;
                if (aws_hpack_decode_integer(decoder, to_decode, 5, size64, &size_complete)) {
                    return AWS_OP_ERR;
                }

                if (!size_complete) {
                    break;
                }
                /* The new maximum size MUST be lower than or equal to the limit determined by the protocol using HPACK.
                 * A value that exceeds this limit MUST be treated as a decoding error. */
                if (*size64 > decoder->dynamic_table_protocol_max_size_setting) {
                    HPACK_LOG(ERROR, decoder, "Dynamic table update size is larger than the protocal setting");
                    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                }
                size_t size = (size_t)*size64;

                HPACK_LOGF(TRACE, decoder, "Dynamic table size update %zu", size);
                if (aws_hpack_resize_dynamic_table(&decoder->context, size)) {
                    return AWS_OP_ERR;
                }

                result->type = AWS_HPACK_DECODE_T_DYNAMIC_TABLE_RESIZE;
                result->data.dynamic_table_resize = size;
                goto handle_complete;
            } break;

            default: {
                AWS_ASSERT(0 && "invalid state");
            } break;
        }
    }

    AWS_ASSERT(to_decode->len == 0);
    result->type = AWS_HPACK_DECODE_T_ONGOING;
    return AWS_OP_SUCCESS;

handle_complete:
    AWS_ASSERT(result->type != AWS_HPACK_DECODE_T_ONGOING);
    decoder->progress_entry.state = HPACK_ENTRY_STATE_INIT;
    return AWS_OP_SUCCESS;
}
