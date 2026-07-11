// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/generator_extended_canonical_2_0_0.h"
#include "mongo/util/modules.h"

#include <string_view>

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
            fmt::format_to(std::back_inserter(buffer), FMT_COMPILE(R"({})"), val);
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
            fmt::format_to(std::back_inserter(buffer),
                           FMT_COMPILE(R"({{"$date":"{}"}})"),
                           std::string_view{DateStringBuffer{}.iso8601(val, _localDate)});
        } else {
            ExtendedCanonicalV200Generator::writeDate(buffer, val);
        }
    }

private:
    bool _localDate;
};
}  // namespace mongo
