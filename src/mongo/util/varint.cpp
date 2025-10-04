/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
