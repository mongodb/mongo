/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "compression_stream.hpp"

#include "util/checksum.hpp"

namespace qpl::ml::compression {

[[nodiscard]] auto compression_stream::bytes_written() const noexcept -> uint32_t {
    return bytes_written_;
}

[[nodiscard]] auto compression_stream::bytes_processed() const noexcept -> uint32_t {
    return bytes_processed_;
}

[[nodiscard]] auto compression_stream::compression_mode() const noexcept -> compression_mode_t {
    return compression_mode_;
}

[[nodiscard]] auto compression_stream::checksum() const noexcept -> util::checksum_accumulator {
    return checksum_;
}

[[nodiscard]] auto compression_stream::is_first_chunk() const noexcept -> bool {
    return chunk_type_.is_first;
}

[[nodiscard]] auto compression_stream::is_last_chunk() const noexcept -> bool {
    return chunk_type_.is_last;
}

[[nodiscard]] auto compression_stream::mini_block_size() const noexcept -> mini_block_size_t {
    return mini_block_size_;
}

void compression_stream::update_checksum(uint8_t *const begin, uint32_t size) noexcept {
    checksum_.crc32 = util::crc32_gzip(begin, begin + size, checksum_.crc32);
}

} // namespace qpl::ml::compression
