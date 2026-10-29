// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_type_terminated.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

Status TerminatedHelper::makeLoadNoTerminalStatus(char c,
                                                  size_t length,
                                                  std::ptrdiff_t debug_offset) {
    str::stream ss;
    ss << "couldn't locate terminal char (" << str::escape(std::string_view(&c, 1))
       << ") in buffer[" << length << "] at offset: " << debug_offset;
    return Status(ErrorCodes::Overflow, ss);
}

Status TerminatedHelper::makeLoadShortReadStatus(char c,
                                                 size_t read,
                                                 size_t length,
                                                 std::ptrdiff_t debug_offset) {
    str::stream ss;
    ss << "only read (" << read << ") bytes. (" << length << ") bytes to terminal char ("
       << str::escape(std::string_view(&c, 1)) << ") at offset: " << debug_offset;

    return Status(ErrorCodes::Overflow, ss);
}

Status TerminatedHelper::makeStoreStatus(char c, size_t length, std::ptrdiff_t debug_offset) {
    str::stream ss;
    ss << "couldn't write terminal char (" << str::escape(std::string_view(&c, 1)) << ") in buffer["
       << length << "] at offset: " << debug_offset;
    return Status(ErrorCodes::Overflow, ss);
}

}  // namespace mongo
