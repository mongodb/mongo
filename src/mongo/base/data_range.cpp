// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range.h"

#include "mongo/util/str.h"

namespace mongo {

Status ConstDataRange::makeOffsetStatus(size_t offset) const {
    str::stream ss;
    ss << "Invalid offset(" << offset << ") past end of buffer[" << length()
       << "] at offset: " << _debug_offset;

    return Status(ErrorCodes::Overflow, ss);
}

Status DataRangeTypeHelper::makeStoreStatus(size_t t_length,
                                            size_t length,
                                            std::ptrdiff_t debug_offset) {
    str::stream ss;
    ss << "buffer size too small to write (" << t_length << ") bytes into buffer[" << length
       << "] at offset: " << debug_offset;
    return Status(ErrorCodes::Overflow, ss);
}

}  // namespace mongo
