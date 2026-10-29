// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstring>
#include <string_view>

namespace mongo {

namespace {

Status makeStoreStatus(std::string_view sdata, size_t length, std::ptrdiff_t debug_offset) {
    str::stream ss;
    ss << "buffer size too small to write std::string_view(" << sdata.size()
       << ") bytes into buffer[" << length << "] at offset: " << debug_offset;
    return Status(ErrorCodes::Overflow, ss);
}
}  // namespace

Status DataType::Handler<std::string_view>::load(std::string_view* sdata,
                                                 const char* ptr,
                                                 size_t length,
                                                 size_t* advanced,
                                                 std::ptrdiff_t debug_offset) {
    if (sdata) {
        *sdata = std::string_view(ptr, length);
    }

    if (advanced) {
        *advanced = length;
    }

    return Status::OK();
}

Status DataType::Handler<std::string_view>::store(std::string_view sdata,
                                                  char* ptr,
                                                  size_t length,
                                                  size_t* advanced,
                                                  std::ptrdiff_t debug_offset) {
    if (sdata.size() > length) {
        return makeStoreStatus(sdata, length, debug_offset);
    }

    if (ptr) {
        std::memcpy(ptr, sdata.data(), sdata.size());
    }

    if (advanced) {
        *advanced = sdata.size();
    }

    return Status::OK();
}

}  // namespace mongo
