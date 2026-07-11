// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_type.h"

#include "mongo/base/error_codes.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {

namespace {
auto makeOverflowStatus(std::string_view action,
                        size_t sizeOfT,
                        size_t length,
                        size_t debug_offset) {
    return Status(
        ErrorCodes::Overflow,
        fmt::format("buffer size too small to {} ({}) bytes out of buffer[{}] at offset: {}",
                    action,
                    sizeOfT,
                    length,
                    debug_offset));
}
}  // namespace

Status DataType::makeTrivialLoadStatus(size_t sizeOfT, size_t length, size_t debug_offset) {
    return makeOverflowStatus("read", sizeOfT, length, debug_offset);
}

Status DataType::makeTrivialStoreStatus(size_t sizeOfT, size_t length, size_t debug_offset) {
    return makeOverflowStatus("write", sizeOfT, length, debug_offset);
}

}  // namespace mongo
