/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/generator_extended_canonical_2_0_0.h"

namespace mongo {
class ExtendedRelaxedV200Generator : private ExtendedCanonicalV200Generator {
public:
    // Use date with local timezone, otherwise UTC.
    explicit ExtendedRelaxedV200Generator(bool localDate) : _localDate(localDate) {}
    using ExtendedCanonicalV200Generator::writeBinData;
    using ExtendedCanonicalV200Generator::writeBool;
    using ExtendedCanonicalV200Generator::writeCode;
    using ExtendedCanonicalV200Generator::writeCodeWithScope;
    using ExtendedCanonicalV200Generator::writeDBRef;
    using ExtendedCanonicalV200Generator::writeDecimal128;
    using ExtendedCanonicalV200Generator::writeMaxKey;
    using ExtendedCanonicalV200Generator::writeMinKey;
    using ExtendedCanonicalV200Generator::writeNull;
    using ExtendedCanonicalV200Generator::writeOID;
    using ExtendedCanonicalV200Generator::writePadding;
    using ExtendedCanonicalV200Generator::writeRegex;
    using ExtendedCanonicalV200Generator::writeString;
    using ExtendedCanonicalV200Generator::writeSymbol;
    using ExtendedCanonicalV200Generator::writeTimestamp;
    using ExtendedCanonicalV200Generator::writeUndefined;

    void writeInt32(fmt::memory_buffer& buffer, int32_t val) const {
        appendTo(buffer, fmt::format_int(val));
    }

    void writeInt64(fmt::memory_buffer& buffer, int64_t val) const {
        appendTo(buffer, fmt::format_int(val));
    }

    void writeDouble(fmt::memory_buffer& buffer, double val) const {
        if (val >= std::numeric_limits<double>::lowest() &&
            val <= std::numeric_limits<double>::max())
            format_to(std::back_inserter(buffer), FMT_COMPILE(R"({})"), val);
        else {
            ExtendedCanonicalV200Generator::writeDouble(buffer, val);
        }
    }

    void writeDate(fmt::memory_buffer& buffer, Date_t val) const {
        // The two cases in which we cannot convert Date_t::millis to an ISO Date string are
        // when the date is too large to format (SERVER-13760), and when the date is before
        // the epoch (SERVER-11273).  Since Date_t internally stores millis as an unsigned
        // long long, despite the fact that it is logically signed (SERVER-8573), this check
        // handles both the case where Date_t::millis is too large, and the case where
        // Date_t::millis is negative (before the epoch).
        if (val.isFormattable()) {
            format_to(std::back_inserter(buffer),
                      FMT_COMPILE(R"({{"$date":"{}"}})"),
                      StringData{DateStringBuffer{}.iso8601(val, _localDate)});
        } else {
            ExtendedCanonicalV200Generator::writeDate(buffer, val);
        }
    }

private:
    bool _localDate;
};
}  // namespace mongo
