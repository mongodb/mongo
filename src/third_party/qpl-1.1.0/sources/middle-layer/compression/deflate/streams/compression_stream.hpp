/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_HPP

#include "util/checksum.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/huffman_table/deflate_huffman_table.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {

class compression_stream {
    template<execution_path_t path>
    friend
    class deflate_state_builder;

public:
    virtual auto write_bytes(const uint8_t *data, uint32_t size) noexcept -> qpl_ml_status = 0;

    virtual void set_source(uint8_t *begin, uint32_t size) noexcept = 0;

    [[nodiscard]] auto bytes_written() const noexcept -> uint32_t;

    [[nodiscard]] auto bytes_processed() const noexcept -> uint32_t;

    [[nodiscard]] auto compression_mode() const noexcept -> compression_mode_t;

    [[nodiscard]] auto header_type() const noexcept -> header_t;

    [[nodiscard]] auto checksum() const noexcept -> util::checksum_accumulator;

    [[nodiscard]] auto is_first_chunk() const noexcept -> bool;

    [[nodiscard]] auto is_last_chunk() const noexcept -> bool;

    [[nodiscard]] auto mini_block_size() const noexcept -> mini_block_size_t;

    void update_checksum(uint8_t *const begin, uint32_t size) noexcept;

protected:
    util::checksum_accumulator checksum_{};

    compression_mode_t   compression_mode_  = dynamic_mode;
    mini_block_size_t    mini_block_size_   = mini_block_size_none;
    chunk_type           chunk_type_{};
    uint32_t             bytes_processed_   = 0;
    uint32_t             bytes_written_     = 0;
};

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_HPP
