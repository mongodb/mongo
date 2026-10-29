// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/varint.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/varint_details.h"

namespace mongo {

Status DataType::Handler<VarInt>::load(
    VarInt* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
    std::uint64_t value;

    const char* newptr = varint_details::Varint::Parse64WithLimit(
        ptr, ptr + length, reinterpret_cast<varint_details::uint64*>(&value));

    if (!newptr) {
        return DataType::makeTrivialLoadStatus(VarInt::kMaxSizeBytes64, length, debug_offset);
    }

    if (t) {
        *t = value;
    }

    if (advanced) {
        *advanced = newptr - ptr;
    }

    return Status::OK();
}

Status DataType::Handler<VarInt>::store(
    const VarInt& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
    // nullptr means it wants to know how much space we want
    if (!ptr) {
        *advanced = VarInt::kMaxSizeBytes64;
        return Status::OK();
    }

    if (VarInt::kMaxSizeBytes64 > length) {
        return DataType::makeTrivialStoreStatus(VarInt::kMaxSizeBytes64, length, debug_offset);
    }

    static_assert(varint_details::Varint::kMax64 == VarInt::kMaxSizeBytes64);

    const char* newptr = varint_details::Varint::Encode64(ptr, t);

    if (advanced) {
        *advanced = newptr - ptr;
    }

    return Status::OK();
}

}  // namespace mongo
