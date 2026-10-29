// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/generator_extended_canonical_2_0_0.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;
class LegacyStrictGenerator : private ExtendedCanonicalV200Generator {
public:
    using ExtendedCanonicalV200Generator::writeBool;
    using ExtendedCanonicalV200Generator::writeNull;

    void writeUndefined(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$undefined" : true })"sv);
    }

    void writeString(fmt::memory_buffer& buffer, std::string_view str) const {
        fmt::format_to(std::back_inserter(buffer), R"("{}")", str::escape(str));
    }

    void writeSymbol(fmt::memory_buffer& buffer, std::string_view symbol) const {
        writeString(buffer, symbol);
    }

    void writeInt32(fmt::memory_buffer& buffer, int32_t val) const {
        writeDouble(buffer, val);
    }

    void writeInt64(fmt::memory_buffer& buffer, int64_t val) const {
        fmt::format_to(std::back_inserter(buffer), R"({{ "$numberLong" : "{}" }})", val);
    }

    void writeDouble(fmt::memory_buffer& buffer, double val) const {
        if (val >= std::numeric_limits<double>::lowest() &&
            val <= std::numeric_limits<double>::max())
            fmt::format_to(std::back_inserter(buffer), R"({:.16g})", val);
        else if (std::isnan(val))
            appendTo(buffer, "NaN"sv);
        else if (std::isinf(val)) {
            if (val > 0)
                appendTo(buffer, "Infinity"sv);
            else
                appendTo(buffer, "-Infinity"sv);
        } else {
            StringBuilder ss;
            ss << "Number " << val << " cannot be represented in JSON";
            uassert(10311, ss.str(), false);
        }
    }

    void writeDecimal128(fmt::memory_buffer& buffer, Decimal128 val) const {
        if (val.isNaN())
            appendTo(buffer, R"({ "$numberDecimal" : "NaN" })"sv);
        else if (val.isInfinite())
            fmt::format_to(std::back_inserter(buffer),
                           R"({{ "$numberDecimal" : "{}" }})",
                           val.isNegative() ? "-Infinity"sv : "Infinity"sv);
        else {
            fmt::format_to(
                std::back_inserter(buffer), R"({{ "$numberDecimal" : "{}" }})", val.toString());
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
            fmt::format_to(
                std::back_inserter(buffer), R"({{ "$date" : "{}" }})", dateToISOStringLocal(val));
        } else {
            fmt::format_to(std::back_inserter(buffer),
                           R"({{ "$date" : {{ "$numberLong" : "{}" }} }})",
                           val.toMillisSinceEpoch());
        }
    }

    void writeDBRef(fmt::memory_buffer& buffer, std::string_view ref, OID id) const {
        fmt::format_to(
            std::back_inserter(buffer), R"({{ "$ref" : "{}", "$id" : "{}" }})", ref, id.toString());
    }

    void writeOID(fmt::memory_buffer& buffer, OID val) const {
        fmt::format_to(std::back_inserter(buffer), R"({{ "$oid" : "{}" }})", val.toString());
    }

    void writeBinData(fmt::memory_buffer& buffer, std::string_view data, BinDataType type) const {
        appendTo(buffer, R"({ "$binary" : ")");
        base64::encode(buffer, data);
        fmt::format_to(std::back_inserter(buffer), R"(", "$type" : "{:02x}" }})", type);
    }

    void writeRegex(fmt::memory_buffer& buffer,
                    std::string_view pattern,
                    std::string_view options) const {
        fmt::format_to(std::back_inserter(buffer),
                       R"({{ "$regex" : "{}", "$options" : "{}" }})",
                       str::escape(pattern),
                       options);
    }

    void writeCode(fmt::memory_buffer& buffer, std::string_view code) const {
        fmt::format_to(std::back_inserter(buffer), R"({{ "$code" : "{}" }})", str::escape(code));
    }

    void writeCodeWithScope(fmt::memory_buffer& buffer,
                            std::string_view code,
                            BSONObj const& scope) const {
        fmt::format_to(
            std::back_inserter(buffer), R"({{ "$code" : "{}", "$scope" : )", str::escape(code));
        scope.jsonStringGenerator(*this, 0, false, buffer);
        appendTo(buffer, R"( })");
    }

    void writeTimestamp(fmt::memory_buffer& buffer, Timestamp val) const {
        fmt::format_to(std::back_inserter(buffer),
                       R"({{ "$timestamp" : {{ "t" : {}, "i" : {} }} }})",
                       val.getSecs(),
                       val.getInc());
    }

    void writeMinKey(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$minKey" : 1 })"sv);
    }

    void writeMaxKey(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$maxKey" : 1 })"sv);
    }

    void writePadding(fmt::memory_buffer& buffer) const {
        buffer.push_back(' ');
    }
};
}  // namespace mongo
