#ifndef AWS_C_CAL_DER_H
#define AWS_C_CAL_DER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/exports.h>

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>

struct aws_der_encoder;
struct aws_der_decoder;

/*
 * Note: encoder/decoder only supports unsigned representations of integers and usage
 * of signed integers might lead to unexpected results.
 * Context: DER spec requires ints to be stored in big endian format with MSB
 * representing signedness. To disambiguate between negative number and big
 * positive number, null byte can be added in front of positive number. DER spec
 * requires representation to be the shortest possible one.
 * During encoding aws_der_encoder_write_unsigned_integer assumes that cursor
 * points to a positive number and will prepend 0 if needed by DER spec to
 * indicate its positive number. Encoder does not support writing negative numbers.
 * Decoder aws_der_encoder_write_unsigned_integer will strip any leading 0 as
 * needed and will error out if der contains negative number.
 * Take special care when integrating with 3p libraries cause they might expect
 * different format. Ex. this format matches what openssl calls bin format
 * (BN_bin2bn) and might not work as expected with openssl mpi format.
 */

enum aws_der_type {
    /* Primitives */
    AWS_DER_BOOLEAN = 0x01,
    AWS_DER_INTEGER = 0x02,
    AWS_DER_BIT_STRING = 0x03,
    AWS_DER_OCTET_STRING = 0x04,
    AWS_DER_NULL = 0x05,
    AWS_DER_OBJECT_IDENTIFIER = 0x06,
    AWS_DER_BMPString = 0x1e,
    AWS_DER_UNICODE_STRING = AWS_DER_BMPString,
    AWS_DER_IA5String = 0x16, /* Unsupported */
    AWS_DER_PrintableString = 0x13,
    AWS_DER_TeletexString = 0x14, /* Unsupported */

    /* Constructed types */
    AWS_DER_SEQUENCE = 0x30,
    AWS_DER_SEQUENCE_OF = AWS_DER_SEQUENCE,
    AWS_DER_SET = 0x31,
    AWS_DER_SET_OF = AWS_DER_SET,
    AWS_DER_UTF8_STRING = 0x0c,

    /* class types */
    AWS_DER_CLASS_UNIVERSAL = 0x00,
    AWS_DER_CLASS_APPLICATION = 0x40,
    AWS_DER_CLASS_CONTEXT = 0x80,
    AWS_DER_CLASS_PRIVATE = 0xc0,

    /* forms */
    AWS_DER_FORM_CONSTRUCTED = 0x20,
    AWS_DER_FORM_PRIMITIVE = 0x00,
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes a DER encoder
 * @param allocator The allocator to use for all allocations within the encoder
 * @param capacity The initial capacity of the encoder scratch buffer (the max size of all encoded TLVs)
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API struct aws_der_encoder *aws_der_encoder_new(struct aws_allocator *allocator, size_t capacity);

/**
 * Cleans up a DER encoder
 * @param encoder The encoder to clean up
 *
 * Note that this destroys the encoder buffer, invalidating any references to the contents given via get_contents()
 */
AWS_CAL_API void aws_der_encoder_destroy(struct aws_der_encoder *encoder);

/**
 * Writes an arbitrarily sized integer to the DER stream
 * @param encoder The encoder to use
 * @param integer A cursor pointing to the integer's memory
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_write_unsigned_integer(struct aws_der_encoder *encoder, struct aws_byte_cursor integer);
/**
 * Writes a boolean to the DER stream
 * @param encoder The encoder to use
 * @param boolean The boolean to write
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_write_boolean(struct aws_der_encoder *encoder, bool boolean);

/**
 * Writes a NULL token to the stream
 * @param encoder The encoder to write to
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_write_null(struct aws_der_encoder *encoder);

/**
 * Writes a BIT_STRING to the stream
 * @param encoder The encoder to use
 * @param bit_string The bit string to encode
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_write_bit_string(struct aws_der_encoder *encoder, struct aws_byte_cursor bit_string);

/**
 * Writes a string to the stream
 * @param encoder The encoder to use
 * @param octet_string The string to encode
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_write_octet_string(
    struct aws_der_encoder *encoder,
    struct aws_byte_cursor octet_string);

/**
 * Begins a SEQUENCE of objects in the DER stream
 * @param encoder The encoder to use
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_begin_sequence(struct aws_der_encoder *encoder);

/**
 * Finishes a SEQUENCE and applies it to the DER stream buffer
 * @param encoder The encoder to update
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_end_sequence(struct aws_der_encoder *encoder);

/**
 * Begins a SET of objects in the DER stream
 * @param encoder The encoder to use
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_begin_set(struct aws_der_encoder *encoder);

/**
 * Finishes a SET and applies it to the DER stream buffer
 * @param encoder The encoder to update
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_end_set(struct aws_der_encoder *encoder);

/**
 * Retrieves the contents of the encoder stream buffer
 * @param encoder The encoder to read from
 * @param cursor The cursor to point at the stream buffer
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_encoder_get_contents(struct aws_der_encoder *encoder, struct aws_byte_cursor *contents);

/**
 * Initializes an DER decoder
 * @param allocator The allocator to use
 * @param input The DER formatted buffer to parse
 * @return Initialized decoder, or NULL
 */
AWS_CAL_API struct aws_der_decoder *aws_der_decoder_new(struct aws_allocator *allocator, struct aws_byte_cursor input);

/**
 * Cleans up a DER encoder
 * @param decoder The encoder to clean up
 */
AWS_CAL_API void aws_der_decoder_destroy(struct aws_der_decoder *decoder);

/**
 * Allows for iteration over the decoded TLVs.
 * @param decoder The decoder to iterate over
 * @return true if there is a tlv to read after advancing, false when done
 */
AWS_CAL_API bool aws_der_decoder_next(struct aws_der_decoder *decoder);

/**
 * The type of the current TLV
 * @param decoder The decoder to inspect
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API enum aws_der_type aws_der_decoder_tlv_type(struct aws_der_decoder *decoder);

/**
 * The size of the current TLV
 * @param decoder The decoder to inspect
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API size_t aws_der_decoder_tlv_length(struct aws_der_decoder *decoder);

/**
 * The number of elements in the current TLV container
 * @param decoder The decoder to inspect
 * @return Number of elements in the current container
 */
AWS_CAL_API size_t aws_der_decoder_tlv_count(struct aws_der_decoder *decoder);

/**
 * Extracts the current TLV string value (BIT_STRING, OCTET_STRING)
 * @param decoder The decoder to extract from
 * @param string The buffer to store the string into
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_decoder_tlv_string(struct aws_der_decoder *decoder, struct aws_byte_cursor *string);

/**
 * Extracts the current TLV INTEGER value (INTEGER)
 * @param decoder The decoder to extract from
 * @param integer The buffer to store the integer into
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_decoder_tlv_unsigned_integer(struct aws_der_decoder *decoder, struct aws_byte_cursor *integer);

/**
 * Extracts the current TLV BOOLEAN value (BOOLEAN)
 * @param decoder The decoder to extract from
 * @param boolean The boolean to store the value into
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_decoder_tlv_boolean(struct aws_der_decoder *decoder, bool *boolean);

/**
 * Extracts the current TLV value as a blob
 * @param decoder The decoder to extract from
 * @param blob The buffer to store the value into
 * @return AWS_OP_ERR if an error occurs, otherwise AWS_OP_SUCCESS
 */
AWS_CAL_API int aws_der_decoder_tlv_blob(struct aws_der_decoder *decoder, struct aws_byte_cursor *blob);

AWS_EXTERN_C_END

#endif
