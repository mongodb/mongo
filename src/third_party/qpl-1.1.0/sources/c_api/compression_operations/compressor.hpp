/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (private C++ API)
 */

#ifndef QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_COMPRESSOR_HPP_
#define QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_COMPRESSOR_HPP_

#include "common/defs.hpp"

/**
 * @anchor DEFLATE_OPERATIONS
 * @name Deflate Operations API
 * @brief Performs data compression/decompression with a deflate method.
 *        This method is fully described here: https://tools.ietf.org/html/rfc1951
 *
 *  | OP_CODE                     | Function                |
 *  |-----------------------------|-------------------------|
 *  | @ref qpl_op_compress        | call to @ref qpl_deflate |
 *  | @ref qpl_op_decompress      | call to @ref qpl_inflate |
 *
 * @note Functions can work with 2 streams:
 *         - A `Source` stream contains data to process
 *         - A `Destination` stream is used to store operation result
 *
 * @note If the operation completes with success the following @ref qpl_job fields will be updated:
 *         - @ref qpl_job.total_in       - updates with a total count of read bytes from the `Source`
 *         - @ref qpl_job.next_in_ptr        - pointer value increases on @ref qpl_job.total_in bytes
 *         - @ref qpl_job.available_in       - set to zero;
 *         - @ref qpl_job.total_out      - updates with a total count of written bytes to the `Destination`
 *         - @ref qpl_job.next_out_ptr       - pointer value increases on @ref qpl_job.total_out bytes
 *         - @ref qpl_job.available_out      - decreases on @ref qpl_job.total_out bytes
 *         - @ref qpl_job.crc           - updates with calculated crc checksum
 *
 * @remark There are extensions that exist to make compression/decompression more configurable.
 * @{
 *
 */

namespace qpl {

/**
 * @brief Compress `Input` stream using deflate method.
 *
 * @param [in,out] job_ptr pointer onto user specified @ref qpl_job
 *
 * @details For operation execution, set the following parameters in `qpl_job_ptr`:
 *      - `Input` properties:
 *          - @ref qpl_job.next_in_ptr             - start address
 *          - @ref qpl_job.available_in            - number of available bytes
 *      - `Output` properties:
 *          - @ref qpl_job.next_out_ptr            - start address of memory region to store the result of the operation
 *          - @ref qpl_job.available_in            - number of available bytes
 *      - General operation options:
 *              - @ref qpl_job.flags          - flags to set operation modes
 *              - @ref qpl_job.level          - compression level
 *      - Advanced operation options:
 *          - User Huffman table:
 *              - @ref qpl_job.huffman_table -
 *          - Indexing mode:
 *              - @ref qpl_job.mini_block_size    - mini-block size
 *              - @ref qpl_job.idx_array      - pointer to index table
 *              - @ref qpl_job.idx_max_size    - max index table size
 *              - @ref qpl_job.idx_num_written - actual index table size
 *
 * <b> `General Deflate flags and options:` </b><br>
 *    Compress job can be performed by chunks:
 *        - To indicate the first chunk for the operation, the @ref QPL_FLAG_FIRST flag must be set;
 *        - To indicate the last chunk for the operation, the @ref QPL_FLAG_LAST flag must be set.
 *    In case when compression is performed by a single chunk, both flags (@ref QPL_FLAG_FIRST and @ref QPL_FLAG_LAST)
 *    must be set.
 *
 *    Compressed stream can have a GZIP header. To create the GZIP header, the @ref QPL_FLAG_GZIP_MODE flag must be set.
 *
 *    Compressor can produce 2 types of blocks:
 *        - `FIXED` block that uses a Huffman table described in the https://tools.ietf.org/html/rfc1951
 *        - `DYNAMIC` block that uses a Huffman table that is built based on the frequency of the symbols.
 *           In most cases data can be compressed better if Dynamic blocks are used.
 *           @ref QPL_FLAG_DYNAMIC_HUFFMAN flag switch compressor into special mode
 *           that produces dynamic deflate blocks
 *
 *    The @ref QPL_FLAG_START_NEW_BLOCK flag is used to finish current deflate block
 *    and start new (possibly with different Huffman table)
 *
 *    The compressor performs post verification of the compressed stream.
 *    The @ref QPL_FLAG_OMIT_VERIFY flag must be set to disable this step.
 *
 * <b> `Huffman Only Mode:` </b><br>
 *    Compressor supports Huffman only mode that implements encoding of literals using Huffman codes
 *    To use this mode, set the following flags:
 *        - @ref QPL_FLAG_FIRST
 *        - @ref QPL_FLAG_LAST
 *        - @ref QPL_FLAG_NO_HDRS
 *        - @ref QPL_FLAG_GEN_LITERALS
 *
 *    `Huffman Only Mode` supports 2 sub-modes:
 *        - `DYNAMIC` generates optimal Huffman table for the stream specified and encodes one. Conditions for enabling:
 *          - the @ref QPL_FLAG_DYNAMIC_HUFFMAN additionally specified flag
 *          - the @ref qpl_job.huffman_table must point onto the @ref qpl_huffman_table_t that
 *            contains the correct Huffman table after the job execution.
 *        - `STATIC` encodes stream using custom Huffman table. Condition for enabling:
 *          - the @ref qpl_job.huffman_table must contain the correct @ref qpl_huffman_table_t
 *
 *     To write literals in the big-endian format, the @ref QPL_FLAG_HUFFMAN_BE must be set additionally.
 *
 *    `Huffman Only Mode` doesn't support 1 sub-modes:
 *        - `FIXED` as default (Huffman table as for general mode). Condition for this case:
 *          - the @ref qpl_job.huffman_table must be `nullptr`
 *
 *
 * <b> `Canned mode:` </b><br>
 *    ...
 *    @todo describe mode
 *    ...
 *
 * <b> `Indexing feature:` </b><br>
 *    Compressed stream can be additionally indexed to provide the @ref qpl_op_decompress operation with
 *    an index table for `Random access `
 *    To enable this feature, the following conditions must be met:
 *        - the @ref qpl_job.mini_block_size must be inited with value other than the @ref qpl_mblk_size_none
 *        - the @ref qpl_job.idx_array must point to memory allocated for the index table
 *        - the @ref qpl_job.idx_max_size must contain the size of the index table
 *        - the @ref qpl_job.idx_num_written must be set to 0 for the @ref QPL_FLAG_FIRST job.
 *
 *    The @ref qpl_job.idx_num_written contains the total count of written indexes after the job is completed
 *         with the @ref QPL_STS_OK status.
 *
 * @return
 *    - @ref QPL_STS_OK
 *    - @ref QPL_STS_OUTPUT_OVERFLOW_ERR
 *    - @ref QPL_STS_INVALID_PARAM_ERR
 *    - @ref QPL_STS_ARCHIVE_HEADER_ERR
 *
 * Example of main usage:
 * @snippet low-level-api/compression_example.cpp QPL_LOW_LEVEL_COMPRESSION_EXAMPLE
 * Example of canned mode usage:
 * @snippet low-level-api/canned_mode_example.cpp QPL_LOW_LEVEL_CANNED_MODE_EXAMPLE
 *
 */
template <qpl::ml::execution_path_t path>
uint32_t perform_compression(qpl_job *const job_ptr) noexcept;

/**
 * @brief Decompresses data that is compressed using deflate format
 *
 * @details For operation execution, set the following parameters in `qpl_job_ptr`:
 *      - `Input` properties:
 *          - @ref qpl_job.next_in_ptr                    - start address
 *          - @ref qpl_job.available_in                   - number of available bytes
 *      - `Output` properties:
 *          - @ref qpl_job.next_out_ptr                   - start address of memory region to store the result
 *                                                          of the operation
 *          - @ref qpl_job.available_in                   - number of available bytes
 *      - Operation options:
 *          - @ref qpl_job.flags                       - flags to set operation modes
 *          - @ref qpl_job.decomp_end_processing       - stop condition for the operation.
 *          - @ref qpl_job.huffman_table - specific Huffman table (see Huffman Only Mode description)
 *          - @ref qpl_job.ignore_start_bits           - the first significant bit in the first byte (see Random access
 *                                                       mode description)
 *          - @ref qpl_job.ignore_end_bits             - the last significant bit in the last byte (see Random access
 *                                                       mode description)
 *
 * <b> `General Inflate flags and options:` </b><br>
 *    Decompress job can be performed by chunks:
 *        - To indicate the first chunk for the operation, the @ref QPL_FLAG_FIRST must be set;
 *        - To indicate the last chunk for the operation, the @ref QPL_FLAG_LAST must be set.
 *    In case when decompression is performed by a single chunk, both flags (@ref QPL_FLAG_FIRST and @ref QPL_FLAG_LAST)
 *    must be set.
 *
 *    Compressed stream can have a GZIP header. To process the header, the @ref QPL_FLAG_GZIP_MODE flag must be set.
 *
 *    Decompression finish criteria can as well be controlled with the content
 *    of the @ref qpl_job.huffman_table field.
 *
 * <b> `Huffman Only Mode:` </b><br>
 *    In case when the input stream is compressed in "Huffman only" mode, the job must be configured with the following flags:
 *        - @ref QPL_FLAG_NO_HDRS
 *        - @ref QPL_FLAG_FIRST
 *        - @ref QPL_FLAG_LAST
 *    In case when literals are written in the big-endian format, the @ref QPL_FLAG_HUFFMAN_BE must be set additionally.
 *    Decompressor must be provided with the corresponding Huffman table in the @ref qpl_job.decomp_end_processing field.
 *
 *    `Huffman Only Mode` doesn't support 1 sub-modes:
 *        - `FIXED` as default (Huffman table as for general mode). Condition for this case:
 *          - the @ref qpl_job.huffman_table must be `nullptr`

 *
 * <b> `Canned mode:` </b><br>
 *    ...
 *    @todo describe mode
 *    ...
 *
 * <b> `Random access  mode:` </b><br>
 *    Deflated stream supports random access if there is an index table for one.
 *
 *    Inflating is performed in two steps:
 *      1. Header reading:
 *          - Extract the `start_bit_offset` and the `end_bit_offset` for the header from the index table
 *          - Set the accurate header position in the following way:
 *              @code
 *                  inflate_job_ptr->ignore_start_bits = start_bit_offset & 7;
 *                  inflate_job_ptr->ignore_end_bits   = 7 & (0 - end_bit_offset);
 *                  inflate_job_ptr->available_in      = ((end_bit_offset + 7) / 8) - (start_bit_offset / 8);
 *                  inflate_job_ptr->next_in_ptr       = first_input_byte_ptr;
 *              @endcode
 *          - Set the @ref QPL_FLAG_FIRST and the @ref QPL_FLAG_RND_ACCESS flags
 *          - Call the `qpl_submit_job(inflate_job_ptr)`
 *
 *      2. Mini-block decompression
 *        - Extract the `start_bit_offset` and the `end_bit_offset` for mini-block from the index table
 *        - Set the accurate header position the same way it is set in the previous step (header reading)
 *        - Set the output buffer for mini-block
 *        - Set the QPL_FLAG_RND_ACCESS flag
 *        - Call the `qpl_submit_job(inflate_job_ptr)`
 *
 * @return
 *    - @ref QPL_STS_OK
 *    - @ref QPL_STS_BEING_PROCESSED
 *    - @ref QPL_STS_MORE_OUTPUT_NEEDED
 *    - @ref QPL_STS_INTL_VERIFY_ERR
 *    - @ref QPL_STS_INVALID_DEFLATE_DATA_ERR
 *    - @ref QPL_STS_INVALID_PARAM_ERR
 *    - @ref QPL_STS_ARCHIVE_UNSUP_METHOD_ERR
 *    - @ref QPL_STS_INFLATE_NEED_DICT_ERR
 *    - @ref QPL_STS_LIBRARY_INTERNAL_ERR
 *
 * Example of main usage:
 * @snippet low-level-api/compression_example.cpp QPL_LOW_LEVEL_COMPRESSION_EXAMPLE
 *
 */
template <qpl::ml::execution_path_t path>
uint32_t perform_decompress(qpl_job *const job_ptr) noexcept;

}

/** @} */

#endif //QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_COMPRESSOR_HPP_
