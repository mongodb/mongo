#ifndef AWS_COMPRESSION_HUFFMAN_TESTING_H
#define AWS_COMPRESSION_HUFFMAN_TESTING_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/compression/huffman.h>

/**
 * The intended use of file is to allow testing of huffman character coders.
 * By doing the following, you can ensure the output of encoders decoders are
 * correct:
 *
 * \code{c}
 * static struct huffman_test_code_point code_points[] = {
 * #include "test_huffman_static_table.def"
 * };
 * \endcode
 *
 * You may then iterate over each code point in the array, and test the
 * following (pseudo-code):
 *
 * \code{c} for (cp in code_points) {
 *     AWS_ASSERT(my_coder->encode(cp.symbol) == cp.pattern);
 *     AWS_ASSERT(my_coder->decode(cp.pattern) == cp.symbol);
 * }
 * \endcode
 */

/**
 * Structure containing all relevant information about a code point
 */
struct huffman_test_code_point {
    uint8_t symbol;
    struct aws_huffman_code code;
};

/**
 * Macro to be used when including a table def file, populates an array of
 * huffman_test_code_points
 */
#define HUFFMAN_CODE(psymbol, pbit_string, pbit_pattern, pnum_bits)                                                    \
    {                                                                                                                  \
        .symbol = (psymbol),                                                                                           \
        .code =                                                                                                        \
            {                                                                                                          \
                .pattern = (pbit_pattern),                                                                             \
                .num_bits = (pnum_bits),                                                                               \
            },                                                                                                         \
    },

/**
 * Function to test a huffman coder to ensure the transitive property applies
 * (input == decode(incode(input)))
 *
 * \param[in]   coder           The symbol coder to test
 * \param[in]   input           The buffer to test
 * \param[in]   size            The size of input
 * \param[in]   encoded_size    The length of the encoded buffer. Pass 0 to skip check.
 * \param[out]  error_string    In case of failure, the error string to report
 *
 * \return AWS_OP_SUCCESS on success, AWS_OP_FAILURE on failure (error_string
 * will be set)
 */
AWS_COMPRESSION_API
int huffman_test_transitive(
    struct aws_huffman_symbol_coder *coder,
    const char *input,
    size_t size,
    size_t encoded_size,
    const char **error_string);

/**
 * Function to test a huffman coder to ensure the transitive property applies
 * when doing partial encodes/decodes (input == decode(incode(input)))
 *
 * \param[in]   coder               The symbol coder to test
 * \param[in]   input               The buffer to test
 * \param[in]   size                The size of input
 * \param[in]   encoded_size        The length of the encoded buffer. Pass 0 to skip check.
 * \param[in]   output_chunk_size   The amount of output to write at once
 * \param[out]  error_string        In case of failure, the error string to
 * report
 *
 * \return AWS_OP_SUCCESS on success, AWS_OP_FAILURE on failure (error_string
 * will be set)
 */
AWS_COMPRESSION_API
int huffman_test_transitive_chunked(
    struct aws_huffman_symbol_coder *coder,
    const char *input,
    size_t size,
    size_t encoded_size,
    size_t output_chunk_size,
    const char **error_string);

#endif /* AWS_COMPRESSION_HUFFMAN_TESTING_H */
