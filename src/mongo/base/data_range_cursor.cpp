// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

namespace mongo {

Status ConstDataRangeCursor::makeAdvanceStatus(size_t advance) const {
    str::stream ss;
    ss << "Invalid advance (" << advance << ") past end of buffer[" << length()
       << "] at offset: " << _debug_offset;

    return Status(ErrorCodes::Overflow, ss);
}

Status DataRangeCursor::makeAdvanceStatus(size_t advance) const {
    str::stream ss;
    ss << "Invalid advance (" << advance << ") past end of buffer[" << length()
       << "] at offset: " << _debug_offset;

    return Status(ErrorCodes::Overflow, ss);
}

}  // namespace mongo
