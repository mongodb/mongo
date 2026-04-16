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

#include "mongo/base/parse_number.h"
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
 * Set of all of the type wrapper keys (all starting with $). We will not allow these to be present
 * anywhere but in the position required by the spec (usually first key).
 */
static const StringDataSet typeWrapperKeys{
    kMinKey, kMaxKey, kUndefined, kNumberInt, kNumberLong,        kNumberDouble, kNumberDecimal,
    kBinary, kUuid,   kOid,       kDate,      kRegularExpression, kDbPointer,    kRef,
    kId,     kCode,   kSymbol,    kScope,     kTimestamp,
};

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

namespace parsers {

void uassertValueType(StringData valuePath, const Value& value, BSONType type) {
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{} value must be of type {}", valuePath, typeName(type)),
            value.getType() == type);
}

void uassertValueType(StringData root, StringData subField, const Value& value, BSONType type) {
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{}.{} value must be of type {}", root, subField, typeName(type)),
            value.getType() == type);
}

/**
 * If DBException with the same code as 'code' is thrown, it is rethrown with the 'msg' as context.
 * If any other exception is thrown, throws a DBException with the given code and message.
 */
template <ErrorCodes::Error EC, typename Callable>
auto rethrowWithErrorCode(Callable&& c, StringData msg) -> decltype(auto) {
    try {
        return c();
    } catch (const ExceptionFor<EC>& e) {
        uasserted(EC, fmt::format("{}: {}", msg, e.reason()));
    } catch (const std::exception&) {
        uasserted(EC, msg);
    }
    throw;
}

Value parseOid(const Value& value) {
    uassertValueType(kOid, value, BSONType::string);
    uassert(ErrorCodes::ConversionFailure,
            kOid + " must be a 24 character hexadecimal string",
            value.getStringData().size() == 24);
    auto oid = rethrowWithErrorCode<ErrorCodes::ConversionFailure>(
        [&value] { return OID(value.getStringData()); }, "$oid value must be a valid objectId");
    return Value(std::move(oid));
}

Value parseSymbol(const Value& value) {
    uassertValueType(kSymbol, value, BSONType::string);
    return Value(BSONSymbol(value.getStringData()));
}

Value parseNumberInt(const Value& value) {
    uassertValueType(kNumberInt, value, BSONType::string);
    int numberInt{0};
    auto ret = NumberParser{}(value.getStringData(), &numberInt);
    uassert(ErrorCodes::ConversionFailure,
            kNumberInt + " value must be a string specifying an integer",
            ret.isOK());
    return Value(numberInt);
}

Value parseNumberLong(const Value& value) {
    uassertValueType(kNumberLong, value, BSONType::string);
    long long numberLong{0};
    auto ret = NumberParser{}(value.getStringData(), &numberLong);
    uassert(ErrorCodes::ConversionFailure,
            kNumberLong + " value must be a string specifying an integer",
            ret.isOK());
    return Value(numberLong);
}

Value parseNumberDouble(const Value& value) {
    uassertValueType(kNumberDouble, value, BSONType::string);
    static constexpr auto msg =
        "$numberDouble value must be a string specifying a number, NaN, Infinity or -Infinity";
    double numberDouble{0};
    auto ret = NumberParser{}(value.getStringData(), &numberDouble);
    uassert(ErrorCodes::ConversionFailure, msg, ret.isOK());

    if (std::isnan(numberDouble)) {
        uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kNaN);
    }
    if (std::isinf(numberDouble)) {
        if (numberDouble < 0) {
            uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kNegInfinity);
        } else {
            uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kPosInfinity);
        }
    }

    return Value(numberDouble);
}

Value parseNumberDecimal(const Value& value) {
    uassertValueType(kNumberDecimal, value, BSONType::string);
    static constexpr auto msg =
        "$numberDecimal value must be a string specifying a number, NaN, Infinity or -Infinity";
    Decimal128 numberDecimal;
    auto ret = NumberParser{}(value.getStringData(), &numberDecimal);
    uassert(ErrorCodes::ConversionFailure, msg, ret.isOK());

    if (numberDecimal.isNaN()) {
        uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kNaN);
    }
    if (numberDecimal.isInfinite()) {
        if (numberDecimal.isNegative()) {
            uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kNegInfinity);
        } else {
            uassert(ErrorCodes::ConversionFailure, msg, value.getStringData() == kPosInfinity);
        }
    }

    return Value(numberDecimal);
}

Value parseBinary(const Value& value) {
    uassertValueType(kBinary, value, BSONType::object);
    auto doc = value.getDocument();

    auto base64Val = doc[kBase64];
    uassertValueType(kBinary, kBase64, base64Val, BSONType::string);

    auto subTypeVal = doc[kSubType];
    uassertValueType(kBinary, kSubType, subTypeVal, BSONType::string);

    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{} object contains additional fields, expected {} and {} only",
                        kBinary,
                        kBase64,
                        kSubType),
            doc.computeSize() == 2);

    std::string subTypeStr = subTypeVal.getString();
    if (subTypeStr.size() == 1) {
        subTypeStr.insert(subTypeStr.begin(), '0');
    }

    static constexpr auto kSubTypeMsg =
        "$binary.subType must be a hex representation of a single byte";
    uassert(ErrorCodes::ConversionFailure, kSubTypeMsg, subTypeStr.size() == 2);
    auto subType = rethrowWithErrorCode<ErrorCodes::ConversionFailure>(
        [&subTypeStr] { return BinDataType(hexblob::decodePair(subTypeStr)); }, kSubTypeMsg);

    static constexpr auto kBase64Msg = "$binary.base64 must be a valid base64 encoded string";
    std::string binData = rethrowWithErrorCode<ErrorCodes::ConversionFailure>(
        [&base64Val] { return base64::decode(base64Val.getStringData()); }, kBase64Msg);
    return Value(BSONBinData(binData.data(), binData.size(), subType));
}

Value parseUuid(const Value& value) {
    uassertValueType(kUuid, value, BSONType::string);

    auto uuid = UUID::parse(value.getStringData());
    uassert(
        ErrorCodes::ConversionFailure, kUuid + " value must be a valid UUID string", uuid.isOK());
    return Value(uuid.getValue());
}

Value parseCode(const Value& value) {
    uassertValueType(kCode, value, BSONType::string);
    return Value(BSONCode(value.getStringData()));
}

Value parseCodeWScope(const Value& value, const Value& scope) {
    uassertValueType(kCode, value, BSONType::string);
    uassertValueType(kScope, scope, BSONType::object);

    auto deserializedScope = deserializeFromExtendedJson(scope);
    uassertValueType(kScope, deserializedScope, BSONType::object);

    return Value(BSONCodeWScope(value.getStringData(), deserializedScope.getDocument().toBson()));
}

Value parseTimestamp(const Value& value) {
    uassertValueType(kTimestamp, value, BSONType::object);
    auto doc = value.getDocument();

    auto tVal = doc["t"];
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{}.t value must be of type {} or {}",
                        kTimestamp,
                        typeName(BSONType::numberInt),
                        typeName(BSONType::numberLong)),
            tVal.getType() == BSONType::numberInt || tVal.getType() == BSONType::numberLong);

    auto iVal = doc["i"];
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{}.i value must be of type {} or {}",
                        kTimestamp,
                        typeName(BSONType::numberInt),
                        typeName(BSONType::numberLong)),
            iVal.getType() == BSONType::numberInt || iVal.getType() == BSONType::numberLong);

    uassert(
        ErrorCodes::ConversionFailure,
        fmt::format(
            "{} object contains additional fields, expected {} and {} only", kTimestamp, "t", "i"),
        doc.computeSize() == 2);

    return Value(Timestamp(Seconds(tVal.getLong()), iVal.getLong()));
}

Value parseRegularExpression(const Value& value) {
    uassertValueType(kRegularExpression, value, BSONType::object);
    auto doc = value.getDocument();

    auto patternVal = doc[kPattern];
    uassertValueType(kRegularExpression, kPattern, patternVal, BSONType::string);

    auto optionsVal = doc[kOptions];
    uassertValueType(kRegularExpression, kOptions, optionsVal, BSONType::string);

    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{} object contains additional fields, expected {} and {} only",
                        kRegularExpression,
                        kPattern,
                        kOptions),
            doc.computeSize() == 2);

    auto regEx = rethrowWithErrorCode<ErrorCodes::ConversionFailure>(
        [&patternVal, &optionsVal] {
            return BSONRegEx(patternVal.getStringData(), optionsVal.getStringData());
        },
        "$regularExpression value must contain a valid regEx and options");

    return Value(regEx);
}

Value parseDbPointer(const Value& value) {
    uassertValueType(kDbPointer, value, BSONType::object);
    auto doc = value.getDocument();

    auto refVal = doc[kRef];
    uassertValueType(kDbPointer, kRef, refVal, BSONType::string);

    auto idVal = doc[kId];
    uassertValueType(kDbPointer, kId, idVal, BSONType::string);
    uassert(ErrorCodes::ConversionFailure,
            kId + " must be a 24 character hexadecimal string",
            idVal.getStringData().size() == 24);

    uassert(
        ErrorCodes::ConversionFailure,
        fmt::format(
            "{} object contains additional fields, expected {} and {} only", kDbPointer, kRef, kId),
        doc.computeSize() == 2);

    auto dbRef = rethrowWithErrorCode<ErrorCodes::ConversionFailure>(
        [&refVal, &idVal] { return BSONDBRef(refVal.getStringData(), OID(idVal.getStringData())); },
        "$dbPointer value must contain a valid $ref and $id");

    return Value(dbRef);
}

Value parseDate(const Value& value) {
    uassert(ErrorCodes::ConversionFailure,
            fmt::format("{} value must be of type {} or {}",
                        kDate,
                        typeName(BSONType::object),
                        typeName(BSONType::string)),
            value.getType() == BSONType::object || value.getType() == BSONType::string);

    if (value.getType() == BSONType::string) {
        auto date = dateFromISOString(value.getStringData());
        uassert(
            ErrorCodes::ConversionFailure, kDate + " must be a valid ISO-8601 string", date.isOK());
        return Value(date.getValue());
    }

    uassert(ErrorCodes::ConversionFailure,
            kDate + " must be a 64-bit signed integer as a string",
            value.getDocument()[kNumberLong].getType() == BSONType::string &&
                value.getDocument().computeSize() == 1);
    return Value(Date_t::fromMillisSinceEpoch(deserializeFromExtendedJson(value).getLong()));
}

Value parseMinKey(const Value& value) {
    uassertValueType(kMinKey, value, BSONType::numberInt);
    uassert(ErrorCodes::ConversionFailure,
            kMinKey + " value must be the integer 1",
            value.getInt() == 1);
    return Value(MINKEY);
}

Value parseMaxKey(const Value& value) {
    uassertValueType(kMaxKey, value, BSONType::numberInt);
    uassert(ErrorCodes::ConversionFailure,
            kMaxKey + " value must be the integer 1",
            value.getInt() == 1);
    return Value(MAXKEY);
}

Value parseUndefined(const Value& value) {
    uassertValueType(kUndefined, value, BSONType::boolean);
    uassert(ErrorCodes::ConversionFailure, kUndefined + " value must be true", value.getBool());
    return Value(BSONUndefined);
}

using ConvertFunction = Value (*)(const Value&);
static const StringDataMap<ConvertFunction> convertFromExtendedJsonMap{
    {kOid, parseOid},
    {kSymbol, parseSymbol},
    {kNumberInt, parseNumberInt},
    {kNumberLong, parseNumberLong},
    {kNumberDouble, parseNumberDouble},
    {kNumberDecimal, parseNumberDecimal},
    {kBinary, parseBinary},
    {kUuid, parseUuid},
    // $code is not matched here because it has two variants
    {kTimestamp, parseTimestamp},
    {kRegularExpression, parseRegularExpression},
    {kDbPointer, parseDbPointer},
    {kDate, parseDate},
    {kMinKey, parseMinKey},
    {kMaxKey, parseMaxKey},
    {kUndefined, parseUndefined},
};

}  // namespace parsers

/**
 * Parses the BSON value from an Extended JSON type wrapper with name 'fieldName'.
 * Returns the parsed value or none on failure.
 * IMPORTANT: The $code wrapper is not supported, since it has two variants.
 */
boost::optional<Value> tryConvertFromSingleKeyExtendedJson(StringData fieldName, Value value) {
    if (fieldName.front() != '$') {
        return boost::none;
    }
    if (auto it = parsers::convertFromExtendedJsonMap.find(fieldName);
        it != parsers::convertFromExtendedJsonMap.end()) {
        return it->second(value);
    }
    return boost::none;
}

/**
 * Callable which is used to transform an Extended JSON-compatible value into the equivalent BSON
 */
struct FromExtendedJsonConverter {
    Value object(const Document& doc) const {
        if (auto it = doc.fieldIterator(); it.more()) {
            auto firstField = it.next();
            if (auto val =
                    tryConvertFromSingleKeyExtendedJson(firstField.first, firstField.second)) {
                // Any additional keys constitute an error.
                uassert(ErrorCodes::ConversionFailure,
                        fmt::format("{} object contains additional fields", firstField.first),
                        !it.more());
                return *val;
            }

            // Match unusual cases which are not matched by tryConvertFromExtendedJson:
            // - {$code: "..."}
            // - {$code: "...", $scope: {...}}
            // - {$scope: "...", $code: {...}}
            if (it.more()) {
                auto secondField = it.next();
                if ((firstField.first == kCode && secondField.first == kScope) ||
                    (firstField.first == kScope && secondField.first == kCode)) {
                    // $code with scope, but $code and $scope fields are unordered.
                    // Verify no third key
                    uassert(ErrorCodes::ConversionFailure,
                            fmt::format("{} object contains additional fields", kCode),
                            !it.more());
                    return parsers::parseCodeWScope(doc[kCode], doc[kScope]);
                }
            } else if (firstField.first == kCode) {
                // $code without scope
                return parsers::parseCode(firstField.second);
            }
        }

        MutableDocument newDoc;
        for (auto it = doc.fieldIterator(); it.more();) {
            auto p = it.next();
            uassert(ErrorCodes::ConversionFailure,
                    fmt::format("{} must be the first field in the object", p.first),
                    !typeWrapperKeys.contains(p.first));
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
        return Value(newArr);
    }

    [[noreturn]] static void throwUnsupported(BSONType type) {
        uasserted(
            ErrorCodes::ConversionFailure,
            fmt::format("Extended JSON cannot contain BSON-only types: {} is not available in JSON",
                        typeName(type)));
    }

    Value operator()(const Value& value) const {
        switch (value.getType()) {
            case BSONType::eoo:
                uasserted(ErrorCodes::BadValue, "Unexpected eoo/missing value");
            case BSONType::minKey:
            case BSONType::binData:
            case BSONType::undefined:
            case BSONType::oid:
            case BSONType::date:
            case BSONType::regEx:
            case BSONType::dbRef:
            case BSONType::code:
            case BSONType::symbol:
            case BSONType::codeWScope:
            case BSONType::timestamp:
            case BSONType::numberDecimal:
            case BSONType::maxKey:
                throwUnsupported(value.getType());
            case BSONType::object:
                return object(value.getDocument());
            case BSONType::array:
                return array(value.getArray());
            case BSONType::string:
            case BSONType::boolean:
            case BSONType::null:
            case BSONType::numberInt:
            case BSONType::numberLong:
            case BSONType::numberDouble:
                return value;
        }
        MONGO_UNREACHABLE;
    }
};

}  // namespace

Value serializeToExtendedJson(const Value& value, bool relaxed) {
    auto result = ToExtendedJsonConverter({.relaxed = relaxed})(value);
    assertDepthLimit(result);
    assertSizeLimit(result);
    return result;
}

Value deserializeFromExtendedJson(const Value& value) {
    auto result = FromExtendedJsonConverter()(value);
    assertDepthLimit(result);
    assertSizeLimit(result);
    return result;
}

}  // namespace mongo::exec::expression::serialize_ejson_utils
