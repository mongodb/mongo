/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_QPLC_COMPRESSION_CONSTS_H_
#define QPL_QPLC_COMPRESSION_CONSTS_H_

#ifdef __cplusplus
extern "C" {
#endif

// ------ Deflate table constants in accordance with rfc 1951 ------ //
#define QPLC_LITERALS_COUNT         256u  /**< Literals count  */
#define QPLC_DEFLATE_LITERALS_COUNT 257u  /**< Deflate literals codes count */
#define QPLC_DEFLATE_MATCHES_COUNT  29u   /**< Deflate matches codes count */
#define QPLC_DEFLATE_OFFSETS_COUNT  30u   /**< Deflate offset codes count */
#define QPLC_DEFLATE_LL_TABLE_SIZE  (QPLC_DEFLATE_LITERALS_COUNT + QPLC_DEFLATE_MATCHES_COUNT) /**< Deflate Literals Lengths count */
#define QPLC_DEFLATE_D_TABLE_SIZE   QPLC_DEFLATE_OFFSETS_COUNT /**< Deflate offset table size */

// ------ Deflate extra bits constants in accordance with rfc 1951 ------ //
#define QPLC_DEFLATE_EXTRA_BITS_START_POSITION   264u  /**< Position in Huffman table where extra bits are used */
#define QPLC_DEFLATE_LENGTH_EXTRA_BITS_INTERVAL  4u    /**< Step for adding extra bits length (matches codes extracting */
#define QPLC_DEFLATE_OFFSETS_EXTRA_BITS_INTERVAL 2u    /**< Step for adding extra bits length (offsets codes extracting */
#define QPLC_DEFLATE_EXTRA_OFFSETS_BEGIN_VALUE   3u    /**< Number of bits that are used for extra Huffman code */

// ------ Deflate algorithm constants in accordance with rfc 1951 ------ //
#define QPLC_DEFLATE_MINIMAL_MATCH_LENGTH         4u    /**< Minimal match length used during match search */
#define QPLC_DEFLATE_BYTES_FOR_HASH_CALCULATION   4u    /**< Number of bytes that is used for hash calculation */
#define QPLC_DEFLATE_MAXIMAL_OFFSET               4096u /**< Maximal offset for match */

// ------ Other constants ------ //
#define QPLC_CODE_LENGTH_BIT_LENGTH     5u    /**< Number of bits that are used to store code length in ISA-L */
#define QPLC_OFFSET_TABLE_SIZE          2u    /**< Length of packed offsets Huffman table */
#define QPLC_HUFFMAN_CODE_LENGTH_MASK   0xFu  /**< 4 bits [3:0] (shift by Huffman code length before use */
#define QPLC_HUFFMAN_CODE_LENGTH_OFFSET 0xFu  /**< Huffman code length offset */
#define QPLC_LENGTH_MASK                (QPLC_HUFFMAN_CODE_LENGTH_MASK << QPLC_HUFFMAN_CODE_LENGTH_OFFSET)
#define QPLC_HUFFMAN_CODE_MASK          ((1u << QPLC_HUFFMAN_CODE_LENGTH_OFFSET) - 1u) /**< Huffman code mask */

#define QPLC_HUFFMAN_CODE_BIT_LENGTH    15u /**< Number of bits used to store Huffman code */
#define QPLC_HUFFMAN_CODE_MAX_LENGTH    16u
#define QPLC_HUFFMAN_CODES_PROPERTIES_TABLE_SIZE (QPLC_HUFFMAN_CODE_MAX_LENGTH - 1u)
#define QPLC_INDEX_TO_CHAR_TABLE_SIZE   257u

#define BFINAL_BIT                      1u    /**< Bfinal bit value in deflate header */

#ifdef __cplusplus
}
#endif

#endif //QPL_QPLC_COMPRESSION_CONSTS_H_
