/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/serialize_ejson_utils.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/base64.h"

namespace mongo::exec::expression::serialize_ejson_utils {

namespace {

/**
 * Assert the depth limit of the resulting value.
 */
void assertDepthLimit(const Value& value) {
    auto maxDepth = BSONDepth::getMaxAllowableDepth();
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("Result exceeds maximum depth limit of {} levels of nesting", maxDepth),
            value.depth(maxDepth) != -1);
}

/**
 * Assert that the value will be under the BSON size limit.
 */
void assertSizeLimit(const Value& value) {
    // Overhead of [a] over a. We use a boolean placeholder and subtract it's size.
    static const auto singleElementArrayOverhead = BSON_ARRAY(true).objsize() - 1;
    try {
        // We cannot validate the BSONObj size of a value unless we serialize it.
        // To do this generically for non-objects, we can wrap in an array and validate against the
        // adjusted size excluding the array overhead.
        Document::validateDocumentBSONSize(BSON_ARRAY(value),
                                           BSONObjMaxUserSize + singleElementArrayOverhead);
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        uasserted(ErrorCodes::ConversionFailure,
                  fmt::format("Result exceeds maximum BSON size limit of {}", BSONObjMaxUserSize));
    }
}

/**
 * Options controlling the behaviour of the Extended JSON conversion.
 */
struct ExtendedJsonOptions {
    /// Enables the Extended JSON v2 Relaxed representation.
    bool relaxed{true};
    /// Enables local time ISO8601 formatting for $date (relaxed format only).
    bool localDate{dateFormatIsLocalTimezone()};
};

/// Formats according to the Extended JSON spec for $uuid.
std::string uuidToFormattedString(StringData data) {
    return UUID::fromCDR(data).toString();
}

// Define string constants to avoid misspelling :)

constexpr StringData kMinKey = "$minKey";
constexpr StringData kMaxKey = "$maxKey";
constexpr StringData kUndefined = "$undefined";
constexpr StringData kNumberInt = "$numberInt";
constexpr StringData kNumberLong = "$numberLong";
constexpr StringData kNumberDouble = "$numberDouble";
constexpr StringData kNumberDecimal = "$numberDecimal";
constexpr StringData kBinary = "$binary";
constexpr StringData kUuid = "$uuid";
constexpr StringData kSubType = "subType";
constexpr StringData kBase64 = "base64";
constexpr StringData kOid = "$oid";
constexpr StringData kDate = "$date";
constexpr StringData kRegularExpression = "$regularExpression";
constexpr StringData kPattern = "pattern";
constexpr StringData kOptions = "options";
constexpr StringData kDbPointer = "$dbPointer";
constexpr StringData kRef = "$ref";
constexpr StringData kId = "$id";
constexpr StringData kCode = "$code";
constexpr StringData kSymbol = "$symbol";
constexpr StringData kNaN = "NaN";
constexpr StringData kPosInfinity = "Infinity";
constexpr StringData kNegInfinity = "-Infinity";
constexpr StringData kScope = "$scope";
constexpr StringData kTimestamp = "$timestamp";

/**
 * Callable which knows how to convert every BSON type to Extended JSON.
 * The format options are passed at construction.
 */
struct ToExtendedJsonConverter {
    Value object(const Document& doc) const {
        MutableDocument newDoc;
        for (auto it = doc.fieldIterator(); it.more();) {
            auto p = it.next();
            newDoc.setField(p.first, (*this)(p.second));
        }
        return newDoc.freezeToValue();
    }

    Value array(const std::vector<Value>& arr) const {
        std::vector<Value> newArr;
        newArr.reserve(arr.size());
        for (auto&& v : arr) {
            newArr.emplace_back((*this)(v));
        }
        return Value(std::move(newArr));
    }

    Value binData(const BSONBinData& binData) const {
        StringData data(static_cast<const char*>(binData.data), binData.length);
        if (binData.type == BinDataType::newUUID && binData.length == UUID::kNumBytes) {
            // We are permitted to but not required to emit $uuid under the spec.
            // However ExtendedCanonicalV200Generator does this, so we do the same here.
            // This may be expected by users and it also has a benefit for us - it allows us to test
            // for equivalence between ExtendedCanonicalV200Generator and this implementation more
            // easily.
            return Value(BSON(kUuid << uuidToFormattedString(data)));
        }

        fmt::memory_buffer buffer;
        base64::encode(buffer, data);
        return Value(
            BSON(kBinary << BSON(kBase64 << StringData(buffer.data(), buffer.size()) << kSubType
                                         << fmt::format("{:x}", binData.type))));
    }

    Value oid(const OID& oid) const {
        static_assert(OID::kOIDSize == 12);
        return Value(BSON(kOid << oid.toString()));
    }

    Value date(Date_t date) const {
        if (opts.relaxed && date.isFormattable()) {
            return Value(
                BSON(kDate << StringData{DateStringBuffer{}.iso8601(date, opts.localDate)}));
        }
        return Value(BSON(kDate << BSON(kNumberLong << fmt::to_string(date.toMillisSinceEpoch()))));
    }

    Value regEx(const char* pattern, const char* options) const {
        return Value(BSON(kRegularExpression << BSON(kPattern << pattern << kOptions << options)));
    }

    Value dbRef(const BSONDBRef& dbRef) const {
        // ExtendedCanonicalV200Generator seems to generate the wrong representation here.
        // Our BSONType::dbRef maps to dbPointer (typeName(BSONType::dbRef) == "dbPointer").
        // There are two types named on the spec page: dbRef ("convention" - not native type)
        // and dbPointer (native type). dbPointer (BSONType::dbRef) should use a $dbPointer
        // wrapper, but our ExtendedCanonicalV200Generator follows the rules set out for the
        // dbRef convention, not the native type.
        return Value(BSON(kDbPointer << BSON(kRef << dbRef.ns << kId << dbRef.oid.toString())));
    }

    Value code(const std::string& code) const {
        return Value(BSON(kCode << code));
    }

    Value symbol(const std::string& symbol) const {
        return Value(BSON(kSymbol << symbol));
    }

    Value codeWScope(const BSONCodeWScope& cws) const {
        // The $scope always uses canonical format.
        ToExtendedJsonConverter scopeConverter = *this;
        scopeConverter.opts.relaxed = false;
        return Value(
            BSON(kCode << cws.code << kScope << scopeConverter.object(Document(cws.scope))));
    }

    Value timestamp(Timestamp ts) const {
        return Value(BSON(kTimestamp << BSON("t" << static_cast<long long>(ts.getSecs()) << "i"
                                                 << static_cast<long long>(ts.getInc()))));
    }

    Value numberInt(int num) const {
        if (opts.relaxed) {
            return Value(num);
        }
        return Value(BSON(kNumberInt << fmt::to_string(num)));
    }

    Value numberLong(long long num) const {
        if (opts.relaxed) {
            return Value(num);
        }
        return Value(BSON(kNumberLong << fmt::to_string(num)));
    }

    Value numberDouble(double num) const {
        if (std::isnan(num)) {
            return Value(BSON(kNumberDouble << kNaN));
        }
        if (std::isinf(num)) {
            if (num < 0) {
                return Value(BSON(kNumberDouble << kNegInfinity));
            } else {
                return Value(BSON(kNumberDouble << kPosInfinity));
            }
        }
        if (opts.relaxed) {
            return Value(num);
        }
        return Value(BSON(kNumberDouble << fmt::to_string(num)));
    }

    Value numberDecimal(Decimal128 num) const {
        if (num.isNaN()) {
            return Value(BSON(kNumberDecimal << kNaN));
        }
        if (num.isInfinite()) {
            if (num.isNegative()) {
                return Value(BSON(kNumberDecimal << kNegInfinity));
            } else {
                return Value(BSON(kNumberDecimal << kPosInfinity));
            }
        }
        return Value(BSON(kNumberDecimal << num.toString()));
    }

    Value operator()(const Value& value) const {
        switch (value.getType()) {
            case BSONType::minKey:
                return Value(BSON(kMinKey << 1));
            case BSONType::eoo:
                uasserted(ErrorCodes::BadValue, "Unexpected eoo/missing value");
            case BSONType::numberDouble:
                return numberDouble(value.getDouble());
            case BSONType::object:
                return object(value.getDocument());
            case BSONType::array:
                return array(value.getArray());
            case BSONType::binData:
                return binData(value.getBinData());
            case BSONType::undefined:
                return Value(BSON(kUndefined << true));
            case BSONType::oid:
                return oid(value.getOid());
            case BSONType::date:
                return date(value.getDate());
            case BSONType::regEx:
                return regEx(value.getRegex(), value.getRegexFlags());
            case BSONType::dbRef:
                return dbRef(value.getDBRef());
            case BSONType::code:
                return code(value.getCode());
            case BSONType::symbol:
                return symbol(value.getSymbol());
            case BSONType::codeWScope:
                return codeWScope(value.getCodeWScope());
            case BSONType::numberInt:
                return numberInt(value.getInt());
            case BSONType::timestamp:
                return timestamp(value.getTimestamp());
            case BSONType::numberLong:
                return numberLong(value.getLong());
            case BSONType::numberDecimal:
                return numberDecimal(value.getDecimal());
            case BSONType::maxKey:
                return Value(BSON(kMaxKey << 1));
            case BSONType::string:
            case BSONType::boolean:
            case BSONType::null:
                return value;
        }
        MONGO_UNREACHABLE;
    }

    ExtendedJsonOptions opts;
};

}  // namespace

Value serializeToExtendedJson(const Value& value, bool relaxed) {
    auto result = ToExtendedJsonConverter({.relaxed = relaxed})(value);
    assertDepthLimit(result);
    assertSizeLimit(result);
    return result;
}

}  // namespace mongo::exec::expression::serialize_ejson_utils
