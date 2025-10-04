/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/compression/huffman.h>

#define BITSIZEOF(val) (sizeof(val) * 8)

static uint8_t MAX_PATTERN_BITS = BITSIZEOF(((struct aws_huffman_code *)0)->pattern);

void aws_huffman_encoder_init(struct aws_huffman_encoder *encoder, struct aws_huffman_symbol_coder *coder) {

    AWS_ASSERT(encoder);
    AWS_ASSERT(coder);

    AWS_ZERO_STRUCT(*encoder);
    encoder->coder = coder;
    encoder->eos_padding = UINT8_MAX;
}

void aws_huffman_encoder_reset(struct aws_huffman_encoder *encoder) {

    AWS_ASSERT(encoder);

    AWS_ZERO_STRUCT(encoder->overflow_bits);
}

void aws_huffman_decoder_init(struct aws_huffman_decoder *decoder, struct aws_huffman_symbol_coder *coder) {

    AWS_ASSERT(decoder);
    AWS_ASSERT(coder);

    AWS_ZERO_STRUCT(*decoder);
    decoder->coder = coder;
}

void aws_huffman_decoder_reset(struct aws_huffman_decoder *decoder) {

    decoder->working_bits = 0;
    decoder->num_bits = 0;
}

void aws_huffman_decoder_allow_growth(struct aws_huffman_decoder *decoder, bool allow_growth) {
    decoder->allow_growth = allow_growth;
}

/* Much of encode is written in a helper function,
   so this struct helps avoid passing all the parameters through by hand */
struct encoder_state {
    struct aws_huffman_encoder *encoder;
    struct aws_byte_buf *output_buf;
    uint8_t working;
    uint8_t bit_pos;
};

/* Helper function to write a single bit_pattern to memory (or working_bits if
 * out of buffer space) */
static int encode_write_bit_pattern(struct encoder_state *state, struct aws_huffman_code bit_pattern) {
    AWS_PRECONDITION(state->output_buf->len < state->output_buf->capacity);

    if (bit_pattern.num_bits == 0) {
        return aws_raise_error(AWS_ERROR_COMPRESSION_UNKNOWN_SYMBOL);
    }

    uint8_t bits_to_write = bit_pattern.num_bits;
    while (bits_to_write > 0) {
        uint8_t bits_for_current = bits_to_write > state->bit_pos ? state->bit_pos : bits_to_write;
        /* Chop off the top 0s and bits that have already been read */
        uint8_t bits_to_cut =
            (BITSIZEOF(bit_pattern.pattern) - bit_pattern.num_bits) + (bit_pattern.num_bits - bits_to_write);

        /* Write the appropiate number of bits to this byte
            Shift to the left to cut any unneeded bits
            Shift to the right to position the bits correctly */
        state->working |= (bit_pattern.pattern << bits_to_cut) >> (MAX_PATTERN_BITS - state->bit_pos);

        bits_to_write -= bits_for_current;
        state->bit_pos -= bits_for_current;

        if (state->bit_pos == 0) {
            /* Save the whole byte */
            aws_byte_buf_write_u8(state->output_buf, state->working);

            state->bit_pos = 8;
            state->working = 0;

            if (state->output_buf->len == state->output_buf->capacity) {
                state->encoder->overflow_bits.num_bits = bits_to_write;

                if (bits_to_write) {
                    /* If buffer is full and there are remaining bits, save them to overflow and return */
                    bits_to_cut += bits_for_current;

                    state->encoder->overflow_bits.pattern =
                        (bit_pattern.pattern << bits_to_cut) >> (MAX_PATTERN_BITS - bits_to_write);

                    return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
                }
            }
        }
    }

    return AWS_OP_SUCCESS;
}

size_t aws_huffman_get_encoded_length(struct aws_huffman_encoder *encoder, struct aws_byte_cursor to_encode) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&to_encode));

    size_t num_bits = 0;

    while (to_encode.len) {
        uint8_t new_byte = 0;
        aws_byte_cursor_read_u8(&to_encode, &new_byte);
        struct aws_huffman_code code_point = encoder->coder->encode(new_byte, encoder->coder->userdata);
        num_bits += code_point.num_bits;
    }

    size_t length = num_bits / 8;

    /* Round up */
    if (num_bits % 8) {
        ++length;
    }

    return length;
}

int aws_huffman_encode(
    struct aws_huffman_encoder *encoder,
    struct aws_byte_cursor *to_encode,
    struct aws_byte_buf *output) {

    AWS_ASSERT(encoder);
    AWS_ASSERT(encoder->coder);
    AWS_ASSERT(to_encode);
    AWS_ASSERT(output);

    struct encoder_state state = {
        .working = 0,
        .bit_pos = 8,
    };
    state.encoder = encoder;
    state.output_buf = output;

    /* Write any bits leftover from previous invocation */
    if (encoder->overflow_bits.num_bits) {
        if (output->len == output->capacity) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }

        if (encode_write_bit_pattern(&state, encoder->overflow_bits)) {
            return AWS_OP_ERR;
        }

        encoder->overflow_bits.num_bits = 0;
    }

    while (to_encode->len) {
        if (output->len == output->capacity) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }

        uint8_t new_byte = 0;
        aws_byte_cursor_read_u8(to_encode, &new_byte);
        struct aws_huffman_code code_point = encoder->coder->encode(new_byte, encoder->coder->userdata);

        if (encode_write_bit_pattern(&state, code_point)) {
            return AWS_OP_ERR;
        }
    }

    /* The following code only runs when the buffer has written successfully */

    /* If whole buffer processed, write EOS */
    if (state.bit_pos != 8) {
        struct aws_huffman_code eos_cp;
        eos_cp.pattern = encoder->eos_padding;
        eos_cp.num_bits = state.bit_pos;
        encode_write_bit_pattern(&state, eos_cp);
        AWS_ASSERT(state.bit_pos == 8);
    }

    return AWS_OP_SUCCESS;
}

/* Decode's reading is written in a helper function,
   so this struct helps avoid passing all the parameters through by hand */
struct huffman_decoder_state {
    struct aws_huffman_decoder *decoder;
    struct aws_byte_cursor *input_cursor;
};

static void decode_fill_working_bits(struct huffman_decoder_state *state) {

    /* Read from bytes in the buffer until there are enough bytes to process */
    while (state->decoder->num_bits < MAX_PATTERN_BITS && state->input_cursor->len) {

        /* Read the appropiate number of bits from this byte */
        uint8_t new_byte = 0;
        aws_byte_cursor_read_u8(state->input_cursor, &new_byte);

        uint64_t positioned = ((uint64_t)new_byte)
                              << (BITSIZEOF(state->decoder->working_bits) - 8 - state->decoder->num_bits);
        state->decoder->working_bits |= positioned;

        state->decoder->num_bits += 8;
    }
}

int aws_huffman_decode(
    struct aws_huffman_decoder *decoder,
    struct aws_byte_cursor *to_decode,
    struct aws_byte_buf *output) {

    AWS_ASSERT(decoder);
    AWS_ASSERT(decoder->coder);
    AWS_ASSERT(to_decode);
    AWS_ASSERT(output);

    struct huffman_decoder_state state;
    state.decoder = decoder;
    state.input_cursor = to_decode;

    /* Measures how much of the input was read */
    size_t bits_left = decoder->num_bits + to_decode->len * 8;

    while (1) {

        decode_fill_working_bits(&state);

        uint8_t symbol;
        uint8_t bits_read = decoder->coder->decode(
            (uint32_t)(decoder->working_bits >> (BITSIZEOF(decoder->working_bits) - MAX_PATTERN_BITS)),
            &symbol,
            decoder->coder->userdata);

        if (bits_read == 0) {
            if (bits_left < MAX_PATTERN_BITS) {
                /* More input is needed to continue */
                return AWS_OP_SUCCESS;
            }
            /* Unknown symbol found */
            return aws_raise_error(AWS_ERROR_COMPRESSION_UNKNOWN_SYMBOL);
        }
        if (bits_read > bits_left) {
            /* Check if the buffer has been overrun.
            Note: because of the check in decode_fill_working_bits,
            the buffer won't actually overrun, instead there will
            be 0's in the bottom of working_bits. */

            return AWS_OP_SUCCESS;
        }

        if (output->len == output->capacity) {
            /* Check if we've hit the end of the output buffer.
             * Grow buffer, or raise error, depending on settings */
            if (decoder->allow_growth) {
                /* Double the capacity */
                if (aws_byte_buf_reserve_relative(output, output->capacity)) {
                    return AWS_OP_ERR;
                }
            } else {
                return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
            }
        }

        bits_left -= bits_read;
        decoder->working_bits <<= bits_read;
        decoder->num_bits -= bits_read;

        /* Store the found symbol */
        aws_byte_buf_write_u8(output, symbol);

        /* Successfully decoded whole buffer */
        if (bits_left == 0) {
            return AWS_OP_SUCCESS;
        }
    }

    /* This case is unreachable */
    AWS_ASSERT(0);
    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}
