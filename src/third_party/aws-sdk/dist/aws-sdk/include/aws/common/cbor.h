#ifndef AWS_COMMON_CBOR_H
#define AWS_COMMON_CBOR_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/**
 * The types use by APIs, not 1:1 with major type.
 * It's an extension for cbor major type in RFC8949 section 3.1
 * Major type 0 - AWS_CBOR_TYPE_UINT
 * Major type 1 - AWS_CBOR_TYPE_NEGINT
 * Major type 2 - AWS_CBOR_TYPE_BYTES/AWS_CBOR_TYPE_INDEF_BYTES_START
 * Major type 3 - AWS_CBOR_TYPE_TEXT/AWS_CBOR_TYPE_INDEF_TEXT_START
 * Major type 4 - AWS_CBOR_TYPE_ARRAY_START/AWS_CBOR_TYPE_INDEF_ARRAY_START
 * Major type 5 - AWS_CBOR_TYPE_MAP_START/AWS_CBOR_TYPE_INDEF_MAP_START
 * Major type 6 - AWS_CBOR_TYPE_TAG
 * Major type 7
 *  - 20/21 - AWS_CBOR_TYPE_BOOL
 *  - 22 - AWS_CBOR_TYPE_NULL
 *  - 23 - AWS_CBOR_TYPE_UNDEFINED
 *  - 25/26/27 - AWS_CBOR_TYPE_FLOAT
 *  - 31 - AWS_CBOR_TYPE_BREAK
 *  - rest of value are not supported.
 */
enum aws_cbor_type {
    AWS_CBOR_TYPE_UNKNOWN = 0,

    AWS_CBOR_TYPE_UINT,
    AWS_CBOR_TYPE_NEGINT,
    AWS_CBOR_TYPE_FLOAT,
    AWS_CBOR_TYPE_BYTES,
    AWS_CBOR_TYPE_TEXT,

    AWS_CBOR_TYPE_ARRAY_START,
    AWS_CBOR_TYPE_MAP_START,

    AWS_CBOR_TYPE_TAG,

    AWS_CBOR_TYPE_BOOL,
    AWS_CBOR_TYPE_NULL,
    AWS_CBOR_TYPE_UNDEFINED,
    AWS_CBOR_TYPE_BREAK,

    AWS_CBOR_TYPE_INDEF_BYTES_START,
    AWS_CBOR_TYPE_INDEF_TEXT_START,
    AWS_CBOR_TYPE_INDEF_ARRAY_START,
    AWS_CBOR_TYPE_INDEF_MAP_START,
};

/**
 * The common tags, refer to RFC8949 section 3.4
 * Expected value type followed by the tag:
 * AWS_CBOR_TAG_STANDARD_TIME - AWS_CBOR_TYPE_TEXT
 * AWS_CBOR_TAG_EPOCH_TIME - AWS_CBOR_TYPE_UINT/AWS_CBOR_TYPE_NEGINT/AWS_CBOR_TYPE_FLOAT
 * AWS_CBOR_TAG_UNSIGNED_BIGNUM - AWS_CBOR_TYPE_BYTES
 * AWS_CBOR_TAG_NEGATIVE_BIGNUM - AWS_CBOR_TYPE_BYTES
 * AWS_CBOR_TAG_DECIMAL_FRACTION - AWS_CBOR_TYPE_ARRAY_START/AWS_CBOR_TYPE_INDEF_ARRAY_START
 **/
#define AWS_CBOR_TAG_STANDARD_TIME 0
#define AWS_CBOR_TAG_EPOCH_TIME 1
#define AWS_CBOR_TAG_UNSIGNED_BIGNUM 2
#define AWS_CBOR_TAG_NEGATIVE_BIGNUM 3
#define AWS_CBOR_TAG_DECIMAL_FRACTION 4

struct aws_cbor_encoder;
struct aws_cbor_decoder;

/*******************************************************************************
 * ENCODE
 ******************************************************************************/

/* Return c-string for aws_cbor_type */
AWS_COMMON_API
const char *aws_cbor_type_cstr(enum aws_cbor_type type);

/**
 * @brief Create a new cbor encoder. Creating a encoder with a temporay buffer.
 * Every aws_cbor_encoder_write_* will encode directly into the buffer to follow the encoded data.
 *
 * @param allocator
 * @return aws_cbor_encoder
 */
AWS_COMMON_API
struct aws_cbor_encoder *aws_cbor_encoder_new(struct aws_allocator *allocator);

AWS_COMMON_API
struct aws_cbor_encoder *aws_cbor_encoder_destroy(struct aws_cbor_encoder *encoder);

/**
 * @brief Get the current encoded data from encoder. The encoded data has the same lifetime as the encoder, and once
 * any other function call invoked for the encoder, the encoded data is no longer valid.
 *
 * @param encoder
 * @return struct aws_byte_cursor from the encoder buffer.
 */
AWS_COMMON_API
struct aws_byte_cursor aws_cbor_encoder_get_encoded_data(const struct aws_cbor_encoder *encoder);

/**
 * @brief Clear the current encoded buffer from encoder.
 *
 * @param encoder
 */
AWS_COMMON_API
void aws_cbor_encoder_reset(struct aws_cbor_encoder *encoder);

/**
 * @brief Encode a AWS_CBOR_TYPE_UINT value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1
 *
 * TODO: maybe add a width of the encoded value.
 *
 * @param encoder
 * @param value value to encode.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_uint(struct aws_cbor_encoder *encoder, uint64_t value);

/**
 * @brief Encode a AWS_CBOR_TYPE_NEGINT value to "smallest possible" in encoder's buffer.
 * It represents (-1 - value).
 *  Referring to RFC8949 section 4.2.1
 *
 *
 * @param encoder
 * @param value The argument to encode to negative integer, which is (-1 - expected_val)
 */
AWS_COMMON_API
void aws_cbor_encoder_write_negint(struct aws_cbor_encoder *encoder, uint64_t value);

/**
 * @brief Encode a AWS_CBOR_TYPE_FLOAT value to "smallest possible", but will not be encoded into half-precision float,
 * as it's not well supported cross languages.
 *
 * To be more specific, it will be encoded into integer/negative/float
 * (Order with priority) when the conversation will not cause precision loss.
 *
 * @param encoder
 * @param value value to encode.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_float(struct aws_cbor_encoder *encoder, double value);

/**
 * @brief Encode a AWS_CBOR_TYPE_BYTES value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1, the length of "from" will be encoded first and then the value of "from" will
 * be followed.
 *
 * @param encoder
 * @param from value to encode.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_bytes(struct aws_cbor_encoder *encoder, struct aws_byte_cursor from);

/**
 * @brief Encode a AWS_CBOR_TYPE_TEXT value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1, the length of "from" will be encoded first and then the value of "from" will
 * be followed.
 *
 * @param encoder
 * @param from value to encode.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_text(struct aws_cbor_encoder *encoder, struct aws_byte_cursor from);

/**
 * @brief Encode a AWS_CBOR_TYPE_ARRAY_START value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1
 *  The "number_entries" is the cbor data items should be followed as the content of the array.
 * Notes: it's user's responsibility to keep the integrity of the array to be encoded.
 *
 * @param encoder
 * @param number_entries The number of data item in array.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_array_start(struct aws_cbor_encoder *encoder, size_t number_entries);

/**
 * @brief Encode a AWS_CBOR_TYPE_MAP_START value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1
 *  The "number_entries" is the number of pair of cbor data items as key and value should be followed as the content of
 * the map.
 *
 * Notes: it's user's responsibility to keep the integrity of the map to be encoded.
 *
 * @param encoder
 * @param number_entries The number of data item in map.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_map_start(struct aws_cbor_encoder *encoder, size_t number_entries);

/**
 * @brief Encode a AWS_CBOR_TYPE_TAG value to "smallest possible" in encoder's buffer.
 *  Referring to RFC8949 section 4.2.1
 * The following cbor data item will be the content of the tagged value.
 * Notes: it's user's responsibility to keep the integrity of the tagged value to follow the RFC8949 section 3.4
 *
 * @param encoder
 * @param tag_number The tag value to encode.
 */
AWS_COMMON_API
void aws_cbor_encoder_write_tag(struct aws_cbor_encoder *encoder, uint64_t tag_number);

/**
 * @brief Encode a simple value AWS_CBOR_TYPE_NULL
 *
 * @param encoder
 */
AWS_COMMON_API
void aws_cbor_encoder_write_null(struct aws_cbor_encoder *encoder);

/**
 * @brief Encode a simple value AWS_CBOR_TYPE_UNDEFINED
 *
 * @param encoder
 */
AWS_COMMON_API
void aws_cbor_encoder_write_undefined(struct aws_cbor_encoder *encoder);

/**
 * @brief Encode a simple value AWS_CBOR_TYPE_BOOL
 *
 * @param encoder
 */
AWS_COMMON_API
void aws_cbor_encoder_write_bool(struct aws_cbor_encoder *encoder, bool value);

/**
 * @brief Encode a simple value AWS_CBOR_TYPE_BREAK
 *
 * Notes: no error checking, it's user's responsibility to track the break
 * to close the corresponding indef_start
 */
AWS_COMMON_API
void aws_cbor_encoder_write_break(struct aws_cbor_encoder *encoder);

/**
 * @brief Encode a AWS_CBOR_TYPE_INDEF_BYTES_START
 *
 * Notes: no error checking, it's user's responsibility to add corresponding data and the break
 * to close the indef_start
 */
AWS_COMMON_API
void aws_cbor_encoder_write_indef_bytes_start(struct aws_cbor_encoder *encoder);
/**
 * @brief Encode a AWS_CBOR_TYPE_INDEF_TEXT_START
 *
 * Notes: no error checking, it's user's responsibility to add corresponding data
 * and the break to close the indef_start
 */
AWS_COMMON_API
void aws_cbor_encoder_write_indef_text_start(struct aws_cbor_encoder *encoder);
/**
 * @brief Encode a AWS_CBOR_TYPE_INDEF_ARRAY_START
 *
 * Notes: no error checking, it's user's responsibility to add corresponding data
 * and the break to close the indef_start
 */
AWS_COMMON_API
void aws_cbor_encoder_write_indef_array_start(struct aws_cbor_encoder *encoder);
/**
 * @brief Encode a AWS_CBOR_TYPE_INDEF_MAP_START
 *
 * Notes: no error checking, it's user's responsibility to add corresponding data
 * and the break to close the indef_start
 */
AWS_COMMON_API
void aws_cbor_encoder_write_indef_map_start(struct aws_cbor_encoder *encoder);

/*******************************************************************************
 * DECODE
 ******************************************************************************/

/**
 * @brief Create a cbor decoder to take src to decode.
 * The typical usage of decoder will be:
 * - If the next element type only accept what expected, `aws_cbor_decoder_pop_next_*`
 * - If the next element type accept different type, invoke `aws_cbor_decoder_peek_type` first, then based on the type
 * to invoke corresponding `aws_cbor_decoder_pop_next_*`
 * - If the next element type doesn't have corrsponding value, specifically: AWS_CBOR_TYPE_NULL,
 * AWS_CBOR_TYPE_UNDEFINED, AWS_CBOR_TYPE_INF_*_START, AWS_CBOR_TYPE_BREAK, call
 * `aws_cbor_decoder_consume_next_single_element` to consume it and continues for further decoding.
 * - To ignore the next data item (the element and the content of it), `aws_cbor_decoder_consume_next_whole_data_item`
 *
 * Note: it's caller's responsibilty to keep the src outlive the decoder.
 *
 * @param allocator
 * @param src   The src data to decode from.
 * @return decoder
 */
AWS_COMMON_API
struct aws_cbor_decoder *aws_cbor_decoder_new(struct aws_allocator *allocator, struct aws_byte_cursor src);

AWS_COMMON_API
struct aws_cbor_decoder *aws_cbor_decoder_destroy(struct aws_cbor_decoder *decoder);

/**
 * @brief  Get the length of the remaining bytes of the source. Once the source was decoded, it will be consumed,
 * and result in decrease of the remaining length of bytes.
 *
 * @param decoder
 * @return The length of bytes remaining of the decoder source.
 */
AWS_COMMON_API
size_t aws_cbor_decoder_get_remaining_length(const struct aws_cbor_decoder *decoder);

/**
 * @brief Decode the next element and store it in the decoder cache if there was no element cached.
 * If there was element cached, just return the type of the cached element.
 *
 * @param decoder
 * @param out_type
 * @return AWS_OP_SUCCESS if succeed, AWS_OP_ERR for any decoding error and corresponding error code will be raised.
 */
AWS_COMMON_API
int aws_cbor_decoder_peek_type(struct aws_cbor_decoder *decoder, enum aws_cbor_type *out_type);

/**
 * @brief Consume the next data item, includes all the content within the data item.
 *
 * As an example for the following cbor, this function will consume all the data
 * as it's only one cbor data item, an indefinite map with 2 <key, value> pair:
 * 0xbf6346756ef563416d7421ff
 * BF           -- Start indefinite-length map
 *   63        -- First key, UTF-8 string length 3
 *      46756e --   "Fun"
 *   F5        -- First value, true
 *   63        -- Second key, UTF-8 string length 3
 *      416d74 --   "Amt"
 *   21        -- Second value, -2
 *   FF        -- "break"
 *
 * Notes: this function will not ensure the data item is well-formed.
 *
 * @param src The src to parse data from
 * @return AWS_OP_SUCCESS successfully consumed the next data item, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_consume_next_whole_data_item(struct aws_cbor_decoder *decoder);

/**
 * @brief Consume the next single element, without the content followed by the element.
 *
 * As an example for the following cbor, this function will only consume the
 * 0xBF, "Start indefinite-length map", not any content of the map represented.
 * The next element to decode will start from 0x63
 * 0xbf6346756ef563416d7421ff
 * BF           -- Start indefinite-length map
 *   63        -- First key, UTF-8 string length 3
 *      46756e --   "Fun"
 *   F5        -- First value, true
 *   63        -- Second key, UTF-8 string length 3
 *      416d74 --   "Amt"
 *   21        -- Second value, -2
 *   FF        -- "break"
 *
 * @param decoder The decoder to parse data from
 * @return AWS_OP_SUCCESS successfully consumed the next element, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_consume_next_single_element(struct aws_cbor_decoder *decoder);

/**
 * @brief Get the next element based on the type. If the next element doesn't match the expected type. Error will be
 * raised. If the next element already been cached, it will consume the cached item when no error was returned.
 * Specifically:
 *  AWS_CBOR_TYPE_UINT - aws_cbor_decoder_pop_next_unsigned_int_val
 *  AWS_CBOR_TYPE_NEGINT - aws_cbor_decoder_pop_next_negative_int_val, it represents (-1 - *out)
 *  AWS_CBOR_TYPE_FLOAT - aws_cbor_decoder_pop_next_float_val
 *  AWS_CBOR_TYPE_BYTES - aws_cbor_decoder_pop_next_bytes_val
 *  AWS_CBOR_TYPE_TEXT - aws_cbor_decoder_pop_next_text_val
 *
 * @param decoder
 * @param out
 * @return AWS_OP_SUCCESS successfully consumed the next element and get the result, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_pop_next_unsigned_int_val(struct aws_cbor_decoder *decoder, uint64_t *out);
AWS_COMMON_API
int aws_cbor_decoder_pop_next_negative_int_val(struct aws_cbor_decoder *decoder, uint64_t *out);
AWS_COMMON_API
int aws_cbor_decoder_pop_next_float_val(struct aws_cbor_decoder *decoder, double *out);
AWS_COMMON_API
int aws_cbor_decoder_pop_next_boolean_val(struct aws_cbor_decoder *decoder, bool *out);
AWS_COMMON_API
int aws_cbor_decoder_pop_next_bytes_val(struct aws_cbor_decoder *decoder, struct aws_byte_cursor *out);
AWS_COMMON_API
int aws_cbor_decoder_pop_next_text_val(struct aws_cbor_decoder *decoder, struct aws_byte_cursor *out);

/**
 * @brief Get the next AWS_CBOR_TYPE_ARRAY_START element. Only consume the AWS_CBOR_TYPE_ARRAY_START element and set the
 * size of array to *out_size, not the content of the array. The next *out_size cbor data items will be the content of
 * the array for a valid cbor data,
 *
 * Notes: For indefinite-length, this function will fail with "AWS_ERROR_CBOR_UNEXPECTED_TYPE". The designed way to
 * handle indefinite-length is:
 * - Get AWS_CBOR_TYPE_INDEF_ARRAY_START from _peek_type
 * - call `aws_cbor_decoder_consume_next_single_element` to pop the indefinite-length start.
 * - Decode the next data item until AWS_CBOR_TYPE_BREAK read.
 *
 * @param decoder
 * @param out_size store the size of array if succeed.
 * @return AWS_OP_SUCCESS successfully consumed the next element and get the result, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_pop_next_array_start(struct aws_cbor_decoder *decoder, uint64_t *out_size);

/**
 * @brief Get the next AWS_CBOR_TYPE_MAP_START element. Only consume the AWS_CBOR_TYPE_MAP_START element and set the
 * size of array to *out_size, not the content of the map. The next *out_size pair of cbor data items as key and value
 * will be the content of the array for a valid cbor data,
 *
 * Notes: For indefinite-length, this function will fail with "AWS_ERROR_CBOR_UNEXPECTED_TYPE". The designed way to
 * handle indefinite-length is:
 * - Get AWS_CBOR_TYPE_INDEF_MAP_START from _peek_type
 * - call `aws_cbor_decoder_consume_next_single_element` to pop the indefinite-length start.
 * - Decode the next data item until AWS_CBOR_TYPE_BREAK read.
 *
 * @param decoder
 * @param out_size store the size of map if succeed.
 * @return AWS_OP_SUCCESS successfully consumed the next element and get the result, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_pop_next_map_start(struct aws_cbor_decoder *decoder, uint64_t *out_size);

/**
 * @brief Get the next AWS_CBOR_TYPE_TAG element. Only consume the AWS_CBOR_TYPE_TAG element and set the
 * tag value to *out_tag_val, not the content of the tagged. The next cbor data item will be the content of the tagged
 * value for a valid cbor data.
 *
 * @param decoder
 * @param out_size store the size of map if succeed.
 * @return AWS_OP_SUCCESS successfully consumed the next element and get the result, otherwise AWS_OP_ERR.
 */
AWS_COMMON_API
int aws_cbor_decoder_pop_next_tag_val(struct aws_cbor_decoder *decoder, uint64_t *out_tag_val);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif // AWS_COMMON_CBOR_H
