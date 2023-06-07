/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_HUFFMAN_TABLE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_HUFFMAN_TABLE_HPP

#include "huff_codes.h"
#include "bitbuf2.h"

#include "compression/huffman_table/deflate_huffman_table.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "common/bit_reverse.hpp"

#include <array>

namespace qpl::ml::compression {
/**
 * @brief Structure that holds Huffman codes for the compression
 *
 * This class is a wrapper over the isa-l's hufftables_icf structure.
 * Hufftable_icf consists of the two huffman tables:
 * 
 * Literals and length(match) table:
 *  - [0:285]   - Huffman Table
 *  - [286:512] - Additional space for the match codes extension (Method: expand_huffman_tables)
 * Offset table:
 *  - [0:29] - Huffman Table
 *  - [30]   - Additional space
 * 
 * Code is bit-reversed
 */
class huffman_table_icf final {
    friend void build_huffman_table_icf(huffman_table_icf &huffman_table, isal_mod_hist *histogram) noexcept;

    friend auto write_huffman_table_icf(BitBuf2 *bit_buffer,
                                        huffman_table_icf &huffman_table,
                                        isal_mod_hist *histogram,
                                        compression_mode_t compression_mode,
                                        uint32_t end_of_block) noexcept -> uint64_t;
public:
    huffman_table_icf() noexcept = default;
    
    huffman_table_icf(hufftables_icf *huffman_table_ptr) noexcept;

    void init_isal_huffman_tables(hufftables_icf *huffman_table_ptr) noexcept;

    void expand_huffman_tables() noexcept;
    auto get_isal_huffman_tables() const noexcept -> hufftables_icf *;

private:
    hufftables_icf                               *huffman_table_;
    uint32_t                                      max_ll_code_index_  = 0;
    uint32_t                                      max_d_code_index_   = 0;
    bool                                          provided_           = false;
};

void build_huffman_table_icf(huffman_table_icf &huffman_table, isal_mod_hist *histogram) noexcept;

auto write_huffman_table_icf(BitBuf2 *bit_buffer,
                             huffman_table_icf &huffman_table,
                             isal_mod_hist *histogram,
                             compression_mode_t compression_mode,
                             uint32_t end_of_block) noexcept -> uint64_t;

void prepare_histogram(isal_mod_hist *histogram) noexcept;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_HUFFMAN_TABLE_HPP
