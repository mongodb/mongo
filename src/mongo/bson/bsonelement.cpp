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


#include "mongo/bson/bsonelement.h"

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_cursor.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/generator_extended_canonical_2_0_0.h"
#include "mongo/bson/generator_extended_relaxed_2_0_0.h"
#include "mongo/bson/generator_legacy_strict.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/duration.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#include <cmath>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using std::string;

const double BSONElement::kLongLongMaxPlusOneAsDouble =
    scalbn(1, std::numeric_limits<long long>::digits);
const long long BSONElement::kLargestSafeLongLongAsDouble =
    scalbn(1, std::numeric_limits<double>::digits);
const long long BSONElement::kSmallestSafeLongLongAsDouble =
    scalbn(-1, std::numeric_limits<double>::digits);

std::string BSONElement::jsonString(JsonStringFormat format,
                                    bool includeSeparator,
                                    bool includeFieldNames,
                                    int pretty,
                                    size_t writeLimit,
                                    BSONObj* outTruncationResult) const {
    fmt::memory_buffer buffer;
    BSONObj truncation =
        jsonStringBuffer(format, includeSeparator, includeFieldNames, pretty, buffer, writeLimit);
    if (outTruncationResult) {
        *outTruncationResult = truncation;
    }
    return fmt::to_string(buffer);
}

BSONObj BSONElement::jsonStringBuffer(JsonStringFormat format,
                                      bool includeSeparator,
                                      bool includeFieldNames,
                                      int pretty,
                                      fmt::memory_buffer& buffer,
                                      size_t writeLimit) const {
    auto withGenerator = [&](auto&& gen) {
        return jsonStringGenerator(
            gen, includeSeparator, includeFieldNames, pretty, buffer, writeLimit);
    };
    if (format == ExtendedCanonicalV2_0_0)
        return withGenerator(ExtendedCanonicalV200Generator());
    else if (format == ExtendedRelaxedV2_0_0)
        return withGenerator(ExtendedRelaxedV200Generator(dateFormatIsLocalTimezone()));
    else if (format == LegacyStrict) {
        return withGenerator(LegacyStrictGenerator());
    } else {
        MONGO_UNREACHABLE;
    }
}

template <typename Generator>
BSONObj BSONElement::_jsonStringGenerator(const Generator& g,
                                          bool includeSeparator,
                                          bool includeFieldNames,
                                          int pretty,
                                          fmt::memory_buffer& buffer,
                                          size_t writeLimit) const {
    size_t before = buffer.size();
    if (includeSeparator)
        buffer.push_back(',');
    if (pretty)
        fmt::format_to(std::back_inserter(buffer), "\n{:<{}}", "", (pretty - 1) * 4);

    if (includeFieldNames) {
        g.writePadding(buffer);
        g.writeString(buffer, fieldNameStringData());
        g.writePadding(buffer);
        buffer.push_back(':');
        if (pretty)
            buffer.push_back(' ');
    }

    g.writePadding(buffer);

    switch (type()) {
        case BSONType::string:
            g.writeString(buffer, valueStringData());
            break;
        case BSONType::symbol:
            g.writeSymbol(buffer, valueStringData());
            break;
        case BSONType::numberLong:
            g.writeInt64(buffer, _numberLong());
            break;
        case BSONType::numberInt:
            g.writeInt32(buffer, _numberInt());
            break;
        case BSONType::numberDouble:
            g.writeDouble(buffer, number());
            break;
        case BSONType::numberDecimal:
            g.writeDecimal128(buffer, numberDecimal());
            break;
        case BSONType::boolean:
            g.writeBool(buffer, boolean());
            break;
        case BSONType::null:
            g.writeNull(buffer);
            break;
        case BSONType::undefined:
            g.writeUndefined(buffer);
            break;
        case BSONType::object: {
            BSONObj truncated =
                embeddedObject().jsonStringGenerator(g, pretty, false, buffer, writeLimit);
            if (!truncated.isEmpty()) {
                BSONObjBuilder builder;
                builder.append(fieldNameStringData(), truncated);
                return builder.obj();
            }
            // return to not check the write limit below, we're not in a leaf
            return truncated;
        }
        case BSONType::array: {
            BSONObj truncated =
                embeddedObject().jsonStringGenerator(g, pretty, true, buffer, writeLimit);
            if (!truncated.isEmpty()) {
                BSONObjBuilder builder;
                builder.append(fieldNameStringData(), truncated);
                return builder.obj();
            }
            // return to not check the write limit below, we're not in a leaf
            return truncated;
        }
        case BSONType::dbRef:
            // valuestrsize() returns the size including the null terminator
            g.writeDBRef(buffer, valueStringData(), OID::from(valuestr() + valuestrsize()));
            break;
        case BSONType::oid:
            g.writeOID(buffer, __oid());
            break;
        case BSONType::binData: {
            ConstDataCursor reader(value());
            const int len = reader.readAndAdvance<LittleEndian<int>>();
            BinDataType type = static_cast<BinDataType>(reader.readAndAdvance<uint8_t>());
            g.writeBinData(buffer, StringData(reader.view(), len), type);
        }

        break;
        case BSONType::date:
            g.writeDate(buffer, date());
            break;
        case BSONType::regEx: {
            StringData pattern(regex());
            g.writeRegex(buffer, pattern, StringData(pattern.data() + pattern.size() + 1));
        } break;
        case BSONType::codeWScope: {
            BSONObj scope = codeWScopeObject();
            if (!scope.isEmpty()) {
                g.writeCodeWithScope(buffer, _asCode(), scope);
                break;
            }
            // fall through if scope is empty
            [[fallthrough]];
        }
        case BSONType::code:
            g.writeCode(buffer, _asCode());
            break;
        case BSONType::timestamp:
            g.writeTimestamp(buffer, timestamp());
            break;
        case BSONType::minKey:
            g.writeMinKey(buffer);
            break;
        case BSONType::maxKey:
            g.writeMaxKey(buffer);
            break;
        default:
            MONGO_UNREACHABLE;
    }
    // If a write limit is enabled and we went over it, record truncation info and roll back buffer.
    if (writeLimit > 0 && buffer.size() > writeLimit) {
        buffer.resize(before);

        BSONObjBuilder builder;
        BSONObjBuilder truncationInfo = builder.subobjStart(fieldNameStringData());
        truncationInfo.append("type"_sd, typeName(type()));
        truncationInfo.append("size"_sd, valuesize());
        truncationInfo.done();
        return builder.obj();
    }
    return BSONObj();
}

BSONObj BSONElement::jsonStringGenerator(ExtendedCanonicalV200Generator const& generator,
                                         bool includeSeparator,
                                         bool includeFieldNames,
                                         int pretty,
                                         fmt::memory_buffer& buffer,
                                         size_t writeLimit) const {
    return _jsonStringGenerator(
        generator, includeSeparator, includeFieldNames, pretty, buffer, writeLimit);
}
BSONObj BSONElement::jsonStringGenerator(ExtendedRelaxedV200Generator const& generator,
                                         bool includeSeparator,
                                         bool includeFieldNames,
                                         int pretty,
                                         fmt::memory_buffer& buffer,
                                         size_t writeLimit) const {
    return _jsonStringGenerator(
        generator, includeSeparator, includeFieldNames, pretty, buffer, writeLimit);
}
BSONObj BSONElement::jsonStringGenerator(LegacyStrictGenerator const& generator,
                                         bool includeSeparator,
                                         bool includeFieldNames,
                                         int pretty,
                                         fmt::memory_buffer& buffer,
                                         size_t writeLimit) const {
    return _jsonStringGenerator(
        generator, includeSeparator, includeFieldNames, pretty, buffer, writeLimit);
}

namespace {

// Compares two string elements using a simple binary compare.
int compareElementStringValues(const BSONElement& leftStr, const BSONElement& rightStr) {
    // we use memcmp as we allow zeros in UTF8 strings
    int lsz = leftStr.valuestrsize();
    int rsz = rightStr.valuestrsize();
    int common = std::min(lsz, rsz);
    int res = memcmp((leftStr.value() + 4), (rightStr.value() + 4), common);
    if (res)
        return res;
    // longer std::string is the greater one
    return lsz - rsz;
}

}  // namespace

int BSONElement::compareElements(const BSONElement& l,
                                 const BSONElement& r,
                                 ComparisonRulesSet rules,
                                 const StringDataComparator* comparator) {
    switch (l.type()) {
        case BSONType::eoo:
        case BSONType::undefined:  // EOO and Undefined are same canonicalType
        case BSONType::null:
        case BSONType::maxKey:
        case BSONType::minKey: {
            auto f = l.canonicalType() - r.canonicalType();
            if (f < 0)
                return -1;
            return f == 0 ? 0 : 1;
        }
        case BSONType::boolean:
            return *l.value() - *r.value();
        case BSONType::timestamp:
            // unsigned compare for timestamps - note they are not really dates but (ordinal +
            // time_t)
            if (l.timestamp() < r.timestamp())
                return -1;
            return l.timestamp() == r.timestamp() ? 0 : 1;
        case BSONType::date:
            // Signed comparisons for Dates.
            {
                const Date_t a = l.Date();
                const Date_t b = r.Date();
                if (a < b)
                    return -1;
                return a == b ? 0 : 1;
            }

        case BSONType::numberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (r.type()) {
                case BSONType::numberInt:
                    return compareInts(l._numberInt(), r._numberInt());
                case BSONType::numberLong:
                    return compareLongs(l._numberInt(), r._numberLong());
                case BSONType::numberDouble:
                    return compareDoubles(l._numberInt(), r._numberDouble());
                case BSONType::numberDecimal:
                    return compareIntToDecimal(l._numberInt(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberLong: {
            switch (r.type()) {
                case BSONType::numberLong:
                    return compareLongs(l._numberLong(), r._numberLong());
                case BSONType::numberInt:
                    return compareLongs(l._numberLong(), r._numberInt());
                case BSONType::numberDouble:
                    return compareLongToDouble(l._numberLong(), r._numberDouble());
                case BSONType::numberDecimal:
                    return compareLongToDecimal(l._numberLong(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberDouble: {
            switch (r.type()) {
                case BSONType::numberDouble:
                    return compareDoubles(l._numberDouble(), r._numberDouble());
                case BSONType::numberInt:
                    return compareDoubles(l._numberDouble(), r._numberInt());
                case BSONType::numberLong:
                    return compareDoubleToLong(l._numberDouble(), r._numberLong());
                case BSONType::numberDecimal:
                    return compareDoubleToDecimal(l._numberDouble(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberDecimal: {
            switch (r.type()) {
                case BSONType::numberDecimal:
                    return compareDecimals(l._numberDecimal(), r._numberDecimal());
                case BSONType::numberInt:
                    return compareDecimalToInt(l._numberDecimal(), r._numberInt());
                case BSONType::numberLong:
                    return compareDecimalToLong(l._numberDecimal(), r._numberLong());
                case BSONType::numberDouble:
                    return compareDecimalToDouble(l._numberDecimal(), r._numberDouble());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::oid:
            return memcmp(l.value(), r.value(), OID::kOIDSize);
        case BSONType::code:
            return compareElementStringValues(l, r);
        case BSONType::symbol:
        case BSONType::string: {
            if (comparator) {
                return comparator->compare(l.valueStringData(), r.valueStringData());
            } else {
                return compareElementStringValues(l, r);
            }
        }
        case BSONType::object:
        case BSONType::array: {
            return l.embeddedObject().woCompare(
                r.embeddedObject(),
                BSONObj(),
                rules | BSONElement::ComparisonRules::kConsiderFieldName,
                comparator);
        }
        case BSONType::dbRef: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case BSONType::binData: {
            int lsz = l.objsize();  // our bin data size in bytes, not including the subtype byte
            int rsz = r.objsize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value() + 4, r.value() + 4, lsz + 1 /*+1 for subtype byte*/);
        }
        case BSONType::regEx: {
            int c = strcmp(l.regex(), r.regex());
            if (c)
                return c;
            return strcmp(l.regexFlags(), r.regexFlags());
        }
        case BSONType::codeWScope: {
            int cmp = StringData(l.codeWScopeCode(), l.codeWScopeCodeLen() - 1)
                          .compare(StringData(r.codeWScopeCode(), r.codeWScopeCodeLen() - 1));
            if (cmp)
                return cmp;

            // When comparing the scope object, we should consider field names. Special string
            // comparison semantics do not apply to strings nested inside the CodeWScope scope
            // object, so we do not pass through the string comparator.
            return l.codeWScopeObject().woCompare(
                r.codeWScopeObject(),
                BSONObj(),
                rules | BSONElement::ComparisonRules::kConsiderFieldName);
        }
    }

    MONGO_UNREACHABLE;
}

std::vector<BSONElement> BSONElement::Array() const {
    chk(BSONType::array);

    std::vector<BSONElement> v;
    DecimalCounter<std::uint32_t> counter(0);
    for (auto element : Obj()) {
        auto fieldName = element.fieldNameStringData();
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid array index field name: \"{}\", expected \"{}\"",
                            fieldName,
                            static_cast<StringData>(counter)),
                fieldName == counter);
        counter++;
        v.push_back(element);
    }
    return v;
}

int BSONElement::woCompare(const BSONElement& elem,
                           ComparisonRulesSet rules,
                           const StringDataComparator* comparator) const {
    if (type() != elem.type()) {
        int lt = (int)canonicalType();
        int rt = (int)elem.canonicalType();
        if (int diff = lt - rt)
            return diff;
    }
    if (rules & ComparisonRules::kConsiderFieldName) {
        if (int diff = fieldNameStringData().compare(elem.fieldNameStringData()))
            return diff;
    }
    return compareElements(*this, elem, rules, comparator);
}

bool BSONElement::binaryEqual(const BSONElement& rhs) const {
    const int elemSize = size();

    if (elemSize != rhs.size()) {
        return false;
    }

    return (elemSize == 0) || (memcmp(_data, rhs.rawdata(), elemSize) == 0);
}

bool BSONElement::binaryEqualValues(const BSONElement& rhs) const {
    // The binaryEqual method above implicitly compares the type, but we need to do so explicitly
    // here. It doesn't make sense to consider to BSONElement objects as binaryEqual if they have
    // the same bit pattern but different types (consider an integer and a double).
    if (type() != rhs.type())
        return false;

    const int valueSize = valuesize();
    if (valueSize != rhs.valuesize()) {
        return false;
    }

    return (valueSize == 0) || (memcmp(value(), rhs.value(), valueSize) == 0);
}

StatusWith<long long> BSONElement::parseIntegerElementToNonNegativeLong() const {
    auto number = parseIntegerElementToLong();
    if (!number.isOK()) {
        return number;
    }

    if (number.getValue() < 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Expected a non-negative number in: " << toString(true, true));
    }

    return number;
}

StatusWith<long long> BSONElement::parseIntegerElementToLong() const {
    if (!isNumber()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Expected a number in: " << toString(true, true));
    }

    long long number = 0;
    if (type() == BSONType::numberDouble) {
        auto eDouble = numberDouble();

        // NaN doubles are rejected.
        if (std::isnan(eDouble)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Expected an integer, but found NaN in: " << toString(true, true));
        }

        // No integral doubles that are too large to be represented as a 64 bit signed integer.
        // We use 'kLongLongMaxAsDouble' because if we just did eDouble > 2^63-1, it would be
        // compared against 2^63. eDouble=2^63 would not get caught that way.
        if (eDouble >= kLongLongMaxPlusOneAsDouble ||
            eDouble < std::numeric_limits<long long>::min()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Cannot represent as a 64-bit integer: " << toString(true, true));
        }

        // This checks if elem is an integral double.
        if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Expected an integer: " << toString(true, true));
        }

        number = numberLong();
    } else if (type() == BSONType::numberDecimal) {
        uint32_t signalingFlags = Decimal128::kNoFlag;
        number = numberDecimal().toLongExact(&signalingFlags);
        if (signalingFlags != Decimal128::kNoFlag) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Cannot represent as a 64-bit integer: " << toString(true, true));
        }
    } else {
        number = numberLong();
    }

    return number;
}

StatusWith<int> BSONElement::parseIntegerElementToInt() const {
    auto parsedLong = parseIntegerElementToLong();
    if (!parsedLong.isOK()) {
        return parsedLong.getStatus();
    }

    auto valueLong = parsedLong.getValue();
    if (valueLong < std::numeric_limits<int>::min() ||
        valueLong > std::numeric_limits<int>::max()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Cannot represent " << toString(true, true) << " in an int"};
    }
    return static_cast<int>(valueLong);
}

StatusWith<int> BSONElement::parseIntegerElementToNonNegativeInt() const {
    auto number = parseIntegerElementToInt();
    if (!number.isOK()) {
        return number;
    }

    if (number.getValue() < 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Expected a non-negative number in: " << toString(true, true));
    }

    return number;
}

BSONObj BSONElement::embeddedObjectUserCheck() const {
    if (MONGO_likely(isABSONObj()))
        return BSONObj(value(), BSONObj::LargeSizeTrait{});
    uasserted(10065,
              str::stream() << "invalid parameter: expected an object (" << fieldName() << ")");
}

BSONObj BSONElement::embeddedObject() const {
    MONGO_verify(isABSONObj());
    return BSONObj(value(), BSONObj::LargeSizeTrait{});
}

BSONObj BSONElement::codeWScopeObject() const {
    MONGO_verify(type() == BSONType::codeWScope);
    int strSizeWNull = ConstDataView(value() + 4).read<LittleEndian<int>>();
    return BSONObj(value() + 4 + 4 + strSizeWNull);
}

// wrap this element up as a singleton object.
BSONObj BSONElement::wrap() const {
    BSONObjBuilder b(size() + 6);
    b.append(*this);
    return b.obj();
}

BSONObj BSONElement::wrap(StringData newName) const {
    BSONObjBuilder b(size() + 6 + newName.size());
    b.appendAs(*this, newName);
    return b.obj();
}

void BSONElement::Val(BSONObj& v) const {
    v = Obj();
}

BSONObj BSONElement::Obj() const {
    return embeddedObjectUserCheck();
}

BSONElement BSONElement::operator[](StringData field) const {
    BSONObj o = Obj();
    return o[field];
}

int BSONElement::computeRegexSize(const char* elem, int fieldNameSize) {
    int8_t type = *elem;
    // The following code handles all special cases: MinKey, MaxKey, RegEx and invalid types.
    if (type == stdx::to_underlying(BSONType::maxKey) ||
        type == stdx::to_underlying(BSONType::minKey))
        return fieldNameSize + 1;

    if (type != stdx::to_underlying(BSONType::regEx)) {
        int err = 10320;  // work around linter
        LOGV2_ERROR(err, "BSONElement: bad type", "type"_attr = zeroPaddedHex(type));
        uasserted(err, "BSONElement: bad type");
    }

    // RegEx is two c-strings back-to-back.
    const char* p = elem + fieldNameSize + 1;
    size_t len1 = strlen(p);
    p = p + len1 + 1;
    size_t len2 = strlen(p);
    return (len1 + 1 + len2 + 1) + fieldNameSize + 1;
}

std::string BSONElement::toString(bool includeFieldName, bool full) const {
    StringBuilder s;
    toString(s, includeFieldName, full, false);
    return s.str();
}

void BSONElement::toString(
    StringBuilder& s, bool includeFieldName, bool full, bool redactValues, int depth) const {
    if (depth > BSONObj::maxToStringRecursionDepth) {
        // check if we want the full/complete string
        if (full) {
            StringBuilder s;
            s << "Reached maximum recursion depth of ";
            s << BSONObj::maxToStringRecursionDepth;
            uassert(16150, s.str(), full != true);
        }
        s << "...";
        return;
    }

    if (includeFieldName && type() != BSONType::eoo)
        s << fieldName() << ": ";

    switch (type()) {
        case BSONType::object:
            return embeddedObject().toString(s, false, full, redactValues, depth + 1);
        case BSONType::array:
            return embeddedObject().toString(s, true, full, redactValues, depth + 1);
        default:
            break;
    }

    if (redactValues) {
        s << "\"###\"";
        return;
    }

    switch (type()) {
        case BSONType::eoo:
            s << "EOO";
            break;
        case BSONType::date:
            s << "new Date(" << date().toMillisSinceEpoch() << ')';
            break;
        case BSONType::regEx: {
            s << "/" << regex() << '/';
            const char* p = regexFlags();
            if (p)
                s << p;
        } break;
        case BSONType::numberDouble:
            s.appendDoubleNice(number());
            break;
        case BSONType::numberLong:
            s << _numberLong();
            break;
        case BSONType::numberInt:
            s << _numberInt();
            break;
        case BSONType::numberDecimal:
            s << _numberDecimal().toString();
            break;
        case BSONType::boolean:
            s << (boolean() ? "true" : "false");
            break;
        case BSONType::undefined:
            s << "undefined";
            break;
        case BSONType::null:
            s << "null";
            break;
        case BSONType::maxKey:
            s << "MaxKey";
            break;
        case BSONType::minKey:
            s << "MinKey";
            break;
        case BSONType::codeWScope:
            s << "CodeWScope( " << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case BSONType::code:
            if (!full && valuestrsize() > 80) {
                s.write(valuestr(), 70);
                s << "...";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
            }
            break;
        case BSONType::symbol:
        case BSONType::string:
            s << '"';
            if (!full && valuestrsize() > 160) {
                s.write(valuestr(), 150);
                s << "...\"";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
                s << '"';
            }
            break;
        case BSONType::dbRef:
            s << "DBRef('" << valuestr() << "',";
            s << OID::from(valuestr() + valuestrsize()) << ')';
            break;
        case BSONType::oid:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BSONType::binData: {
            int len;
            const char* data = binDataClean(len);
            // If the BinData is a correctly sized newUUID, display it as such.
            if (binDataType() == newUUID && len == 16) {
                StringData sd(data, len);
                // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
                s << fmt::format("UUID(\"{}-{}-{}-{}-{}\")",
                                 hexblob::encodeLower(sd.substr(0, 4)),
                                 hexblob::encodeLower(sd.substr(4, 2)),
                                 hexblob::encodeLower(sd.substr(6, 2)),
                                 hexblob::encodeLower(sd.substr(8, 2)),
                                 hexblob::encodeLower(sd.substr(10, 6)));
                break;
            }
            s << "BinData(" << binDataType() << ", ";
            if (!full && len > 80) {
                s << hexblob::encode(data, 70) << "...)";
            } else {
                s << hexblob::encode(data, std::max(len, 0)) << ")";
            }
        } break;

        case BSONType::timestamp: {
            // Convert from Milliseconds to Seconds for consistent Timestamp printing.
            auto secs = duration_cast<Seconds>(timestampTime().toDurationSinceEpoch());
            s << "Timestamp(" << secs.count() << ", " << timestampInc() << ")";
        } break;
        default:
            s << "?type=" << type();
            break;
    }
}

std::string BSONElement::_asCode() const {
    switch (type()) {
        case BSONType::string:
        case BSONType::code:
            return std::string{valueStringData()};
        case BSONType::codeWScope:
            return std::string(codeWScopeCode(),
                               ConstDataView(valuestr()).read<LittleEndian<int>>() - 1);
        default:
            LOGV2(20100, "can't convert type: {int_type} to code", "int_type"_attr = (int)(type()));
    }
    uassert(10062, "not code", 0);
    return "";
}

std::ostream& operator<<(std::ostream& s, const BSONElement& e) {
    return s << e.toString();
}

StringBuilder& operator<<(StringBuilder& s, const BSONElement& e) {
    e.toString(s);
    return s;
}

bool BSONElement::coerce(std::string* out) const {
    if (type() != BSONType::string)
        return false;
    *out = String();
    return true;
}

bool BSONElement::coerce(int* out) const {
    if (!isNumber())
        return false;
    *out = numberInt();
    return true;
}

bool BSONElement::coerce(long long* out) const {
    if (!isNumber())
        return false;
    *out = numberLong();
    return true;
}

bool BSONElement::coerce(double* out) const {
    if (!isNumber())
        return false;
    *out = numberDouble();
    return true;
}

bool BSONElement::coerce(Decimal128* out) const {
    if (!isNumber())
        return false;
    *out = numberDecimal();
    return true;
}

bool BSONElement::coerce(bool* out) const {
    *out = trueValue();
    return true;
}

bool BSONElement::coerce(std::vector<std::string>* out) const {
    if (type() != BSONType::array)
        return false;
    return Obj().coerceVector<std::string>(out);
}

template <typename T>
bool BSONObj::coerceVector(std::vector<T>* out) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        T t;
        if (!e.coerce(&t))
            return false;
        out->push_back(t);
    }
    return true;
}

/**
 * Types used to represent BSONElement memory in the Visual Studio debugger
 */
#if defined(_MSC_VER)
struct BSONElementData {
    char type;
    char name;
} bsonElementDataInstance;

struct BSONElementBinaryType {
    int32_t size;
    uint8_t subtype;
} bsonElementBinaryType;
struct BSONElementRegexType {
} bsonElementRegexType;
struct BSONElementDBRefType {
} bsonElementDBPointerType;
struct BSONElementCodeWithScopeType {
} bsonElementCodeWithScopeType;
#endif  // defined(_MSC_VER)

}  // namespace mongo
