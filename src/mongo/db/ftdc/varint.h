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

#pragma once

#include <cstddef>
#include <cstdint>

#include "mongo/base/data_type.h"
#include "mongo/base/status.h"

namespace mongo {
/**
 * Methods to compress and decompress 64-bit integers into variable integers
 *
 * Uses a technique described here:
 * S. Buttcher, C. L. A. Clarke, and G. V. Cormack.
 *  Information Retrieval: Implementing and Evaluating Search Engines. MIT Press, Cambridge, MA,
 *  2010
 */
struct FTDCVarInt {
    /**
    * Maximum number of bytes an integer can compress to
    */
    static const std::size_t kMaxSizeBytes64 = 10;

    FTDCVarInt() = default;
    FTDCVarInt(std::uint64_t t) : _value(t) {}

    operator std::uint64_t() const {
        return _value;
    }

private:
    std::uint64_t _value{0};
};

template <>
struct DataType::Handler<FTDCVarInt> {
    /**
     * Compress a 64-bit integer and return the new buffer position.
     *
     * end should be the byte after the end of the buffer.
     *
     * Return nullptr for bad encoded data.
     */
    static Status load(FTDCVarInt* t,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset);

    /**
     * Compress a 64-bit integer and return the new buffer position
     */
    static Status store(const FTDCVarInt& t,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset);

    static FTDCVarInt defaultConstruct() {
        return 0;
    }
};

}  // namespace mongo
