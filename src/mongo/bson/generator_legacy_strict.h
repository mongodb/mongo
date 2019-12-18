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
#include "mongo/bson/util/builder.h"
#include "mongo/util/str.h"

#include <fmt/format.h>

namespace mongo {
class LegacyStrictGenerator : private ExtendedCanonicalV200Generator {
public:
    using ExtendedCanonicalV200Generator::writeBool;
    using ExtendedCanonicalV200Generator::writeNull;

    void writeUndefined(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$undefined" : true })"_sd);
    }

    void writeString(fmt::memory_buffer& buffer, StringData str) const {
        fmt::format_to(buffer, R"("{}")", str::escape(str));
    }

    void writeSymbol(fmt::memory_buffer& buffer, StringData symbol) const {
        writeString(buffer, symbol);
    }

    void writeInt32(fmt::memory_buffer& buffer, int32_t val) const {
        writeDouble(buffer, val);
    }

    void writeInt64(fmt::memory_buffer& buffer, int64_t val) const {
        fmt::format_to(buffer, R"({{ "$numberLong" : "{}" }})", val);
    }

    void writeDouble(fmt::memory_buffer& buffer, double val) const {
        if (val >= std::numeric_limits<double>::lowest() &&
            val <= std::numeric_limits<double>::max())
            fmt::format_to(buffer, R"({:.16g})", val);
        else if (std::isnan(val))
            appendTo(buffer, "NaN"_sd);
        else if (std::isinf(val)) {
            if (val > 0)
                appendTo(buffer, "Infinity"_sd);
            else
                appendTo(buffer, "-Infinity"_sd);
        } else {
            StringBuilder ss;
            ss << "Number " << val << " cannot be represented in JSON";
            uassert(10311, ss.str(), false);
        }
    }

    void writeDecimal128(fmt::memory_buffer& buffer, Decimal128 val) const {
        if (val.isNaN())
            appendTo(buffer, R"({ "$numberDecimal" : "NaN" })"_sd);
        else if (val.isInfinite())
            fmt::format_to(buffer,
                           R"({{ "$numberDecimal" : "{}" }})",
                           val.isNegative() ? "-Infinity"_sd : "Infinity"_sd);
        else {
            fmt::format_to(buffer, R"({{ "$numberDecimal" : "{}" }})", val.toString());
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
            fmt::format_to(buffer, R"({{ "$date" : "{}" }})", dateToISOStringLocal(val));
        } else {
            fmt::format_to(
                buffer, R"({{ "$date" : {{ "$numberLong" : "{}" }} }})", val.toMillisSinceEpoch());
        }
    }

    void writeDBRef(fmt::memory_buffer& buffer, StringData ref, OID id) const {
        fmt::format_to(buffer, R"({{ "$ref" : "{}", "$id" : "{}" }})", ref, id.toString());
    }

    void writeOID(fmt::memory_buffer& buffer, OID val) const {
        fmt::format_to(buffer, R"({{ "$oid" : "{}" }})", val.toString());
    }

    void writeBinData(fmt::memory_buffer& buffer, StringData data, BinDataType type) const {
        appendTo(buffer, R"({ "$binary" : ")");
        base64::encode(buffer, data);
        fmt::format_to(buffer, R"(", "$type" : "{:02x}" }})", type);
    }

    void writeRegex(fmt::memory_buffer& buffer, StringData pattern, StringData options) const {
        fmt::format_to(
            buffer, R"({{ "$regex" : "{}", "$options" : "{}" }})", str::escape(pattern), options);
    }

    void writeCode(fmt::memory_buffer& buffer, StringData code) const {
        fmt::format_to(buffer, R"({{ "$code" : "{}" }})", str::escape(code));
    }

    void writeCodeWithScope(fmt::memory_buffer& buffer,
                            StringData code,
                            BSONObj const& scope) const {
        fmt::format_to(buffer, R"({{ "$code" : "{}", "$scope" : )", str::escape(code));
        scope.jsonStringGenerator(*this, 0, false, buffer);
        appendTo(buffer, R"( })");
    }

    void writeTimestamp(fmt::memory_buffer& buffer, Timestamp val) const {
        fmt::format_to(buffer,
                       R"({{ "$timestamp" : {{ "t" : {}, "i" : {} }} }})",
                       val.getSecs(),
                       val.getInc());
    }

    void writeMinKey(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$minKey" : 1 })"_sd);
    }

    void writeMaxKey(fmt::memory_buffer& buffer) const {
        appendTo(buffer, R"({ "$maxKey" : 1 })"_sd);
    }

    void writePadding(fmt::memory_buffer& buffer) const {
        buffer.push_back(' ');
    }
};
}  // namespace mongo
