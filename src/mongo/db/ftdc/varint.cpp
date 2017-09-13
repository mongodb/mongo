/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/db/ftdc/varint.h"

#include <third_party/s2/util/coding/varint.h>

#include "mongo/util/assert_util.h"

namespace mongo {

Status DataType::Handler<FTDCVarInt>::load(
    FTDCVarInt* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
    std::uint64_t value;

    const char* newptr =
        Varint::Parse64WithLimit(ptr, ptr + length, reinterpret_cast<uint64*>(&value));

    if (!newptr) {
        return DataType::makeTrivialLoadStatus(FTDCVarInt::kMaxSizeBytes64, length, debug_offset);
    }

    if (t) {
        *t = value;
    }

    if (advanced) {
        *advanced = newptr - ptr;
    }

    return Status::OK();
}

Status DataType::Handler<FTDCVarInt>::store(
    const FTDCVarInt& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
    // nullptr means it wants to know how much space we want
    if (!ptr) {
        *advanced = FTDCVarInt::kMaxSizeBytes64;
        return Status::OK();
    }

    if (FTDCVarInt::kMaxSizeBytes64 > length) {
        return DataType::makeTrivialStoreStatus(FTDCVarInt::kMaxSizeBytes64, length, debug_offset);
    }

    // Use a dassert since static_assert does not work because the expression does not have a
    // constant value
    dassert(Varint::kMax64 == FTDCVarInt::kMaxSizeBytes64);

    const char* newptr = Varint::Encode64(ptr, t);

    if (advanced) {
        *advanced = newptr - ptr;
    }

    return Status::OK();
}

}  // namespace mongo
