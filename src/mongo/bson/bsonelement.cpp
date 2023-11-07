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
#include <cmath>
#include <ostream>

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
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/duration.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using std::dec;
using std::hex;
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
        fmt::format_to(buffer, "\n{:<{}}", "", (pretty - 1) * 4);

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
        case mongo::String:
            g.writeString(buffer, valueStringData());
            break;
        case Symbol:
            g.writeSymbol(buffer, valueStringData());
            break;
        case NumberLong:
            g.writeInt64(buffer, _numberLong());
            break;
        case NumberInt:
            g.writeInt32(buffer, _numberInt());
            break;
        case NumberDouble:
            g.writeDouble(buffer, number());
            break;
        case NumberDecimal:
            g.writeDecimal128(buffer, numberDecimal());
            break;
        case mongo::Bool:
            g.writeBool(buffer, boolean());
            break;
        case jstNULL:
            g.writeNull(buffer);
            break;
        case Undefined:
            g.writeUndefined(buffer);
            break;
        case Object: {
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
        case mongo::Array: {
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
        case DBRef:
            // valuestrsize() returns the size including the null terminator
            g.writeDBRef(buffer, valueStringData(), OID::from(valuestr() + valuestrsize()));
            break;
        case jstOID:
            g.writeOID(buffer, __oid());
            break;
        case BinData: {
            ConstDataCursor reader(value());
            const int len = reader.readAndAdvance<LittleEndian<int>>();
            BinDataType type = static_cast<BinDataType>(reader.readAndAdvance<uint8_t>());
            g.writeBinData(buffer, StringData(reader.view(), len), type);
        }

        break;
        case mongo::Date:
            g.writeDate(buffer, date());
            break;
        case RegEx: {
            StringData pattern(regex());
            g.writeRegex(buffer, pattern, StringData(pattern.rawData() + pattern.size() + 1));
        } break;
        case CodeWScope: {
            BSONObj scope = codeWScopeObject();
            if (!scope.isEmpty()) {
                g.writeCodeWithScope(buffer, _asCode(), scope);
                break;
            }
            // fall through if scope is empty
            [[fallthrough]];
        }
        case Code:
            g.writeCode(buffer, _asCode());
            break;
        case bsonTimestamp:
            g.writeTimestamp(buffer, timestamp());
            break;
        case MinKey:
            g.writeMinKey(buffer);
            break;
        case MaxKey:
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
        case BSONType::EOO:
        case BSONType::Undefined:  // EOO and Undefined are same canonicalType
        case BSONType::jstNULL:
        case BSONType::MaxKey:
        case BSONType::MinKey: {
            auto f = l.canonicalType() - r.canonicalType();
            if (f < 0)
                return -1;
            return f == 0 ? 0 : 1;
        }
        case BSONType::Bool:
            return *l.value() - *r.value();
        case BSONType::bsonTimestamp:
            // unsigned compare for timestamps - note they are not really dates but (ordinal +
            // time_t)
            if (l.timestamp() < r.timestamp())
                return -1;
            return l.timestamp() == r.timestamp() ? 0 : 1;
        case BSONType::Date:
            // Signed comparisons for Dates.
            {
                const Date_t a = l.Date();
                const Date_t b = r.Date();
                if (a < b)
                    return -1;
                return a == b ? 0 : 1;
            }

        case BSONType::NumberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (r.type()) {
                case NumberInt:
                    return compareInts(l._numberInt(), r._numberInt());
                case NumberLong:
                    return compareLongs(l._numberInt(), r._numberLong());
                case NumberDouble:
                    return compareDoubles(l._numberInt(), r._numberDouble());
                case NumberDecimal:
                    return compareIntToDecimal(l._numberInt(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberLong: {
            switch (r.type()) {
                case NumberLong:
                    return compareLongs(l._numberLong(), r._numberLong());
                case NumberInt:
                    return compareLongs(l._numberLong(), r._numberInt());
                case NumberDouble:
                    return compareLongToDouble(l._numberLong(), r._numberDouble());
                case NumberDecimal:
                    return compareLongToDecimal(l._numberLong(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberDouble: {
            switch (r.type()) {
                case NumberDouble:
                    return compareDoubles(l._numberDouble(), r._numberDouble());
                case NumberInt:
                    return compareDoubles(l._numberDouble(), r._numberInt());
                case NumberLong:
                    return compareDoubleToLong(l._numberDouble(), r._numberLong());
                case NumberDecimal:
                    return compareDoubleToDecimal(l._numberDouble(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberDecimal: {
            switch (r.type()) {
                case NumberDecimal:
                    return compareDecimals(l._numberDecimal(), r._numberDecimal());
                case NumberInt:
                    return compareDecimalToInt(l._numberDecimal(), r._numberInt());
                case NumberLong:
                    return compareDecimalToLong(l._numberDecimal(), r._numberLong());
                case NumberDouble:
                    return compareDecimalToDouble(l._numberDecimal(), r._numberDouble());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::jstOID:
            return memcmp(l.value(), r.value(), OID::kOIDSize);
        case BSONType::Code:
            return compareElementStringValues(l, r);
        case BSONType::Symbol:
        case BSONType::String: {
            if (comparator) {
                return comparator->compare(l.valueStringData(), r.valueStringData());
            } else {
                return compareElementStringValues(l, r);
            }
        }
        case BSONType::Object:
        case BSONType::Array: {
            return l.embeddedObject().woCompare(
                r.embeddedObject(),
                BSONObj(),
                rules | BSONElement::ComparisonRules::kConsiderFieldName,
                comparator);
        }
        case BSONType::DBRef: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case BSONType::BinData: {
            int lsz = l.objsize();  // our bin data size in bytes, not including the subtype byte
            int rsz = r.objsize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value() + 4, r.value() + 4, lsz + 1 /*+1 for subtype byte*/);
        }
        case BSONType::RegEx: {
            int c = strcmp(l.regex(), r.regex());
            if (c)
                return c;
            return strcmp(l.regexFlags(), r.regexFlags());
        }
        case BSONType::CodeWScope: {
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
    chk(mongo::Array);

    std::vector<BSONElement> v;
    DecimalCounter<std::uint32_t> counter(0);
    for (auto element : Obj()) {
        auto fieldName = element.fieldNameStringData();
        uassert(ErrorCodes::BadValue,
                fmt::format(
                    "Invalid array index field name: \"{}\", expected \"{}\"", fieldName, counter),
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

    return (elemSize == 0) || (memcmp(data, rhs.rawdata(), elemSize) == 0);
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
    if (type() == BSONType::NumberDouble) {
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
    } else if (type() == BSONType::NumberDecimal) {
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
    std::stringstream ss;
    ss << "invalid parameter: expected an object (" << fieldName() << ")";
    uasserted(10065, ss.str());
    return BSONObj();  // never reachable
}

BSONObj BSONElement::embeddedObject() const {
    MONGO_verify(isABSONObj());
    return BSONObj(value(), BSONObj::LargeSizeTrait{});
}

BSONObj BSONElement::codeWScopeObject() const {
    MONGO_verify(type() == CodeWScope);
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

namespace {
MONGO_COMPILER_NOINLINE void msgAssertedBadType [[noreturn]] (const char* data) {
    // We intentionally read memory that may be out of the allocated memory's boundary, so do not
    // do this when the address sanitizer is enabled. We do this in an attempt to log as much
    // context about the failure, even if that risks undefined behavior or a segmentation fault.
#if !__has_feature(address_sanitizer)
    bool logMemory = true;
#else
    bool logMemory = false;
#endif

    str::stream output;
    if (!logMemory) {
        output << fmt::format("BSONElement: bad type {0:d} @ {1:p}", *data, data);
    } else {
        // To reduce the risk of a segmentation fault, only print the bytes in the 32-bit aligned
        // block in which the address is located (i.e. round down to the lowest multiple of 32). The
        // hope is that it's safe to read memory that may fall within the same cache line. Generate
        // a mask to zero-out the last bits for a block-aligned address.
        // Ex: Inverse of 0x1F (32 - 1) looks like 0xFFFFFFE0, and ANDed with the pointer, zeroes
        // the lowest 5 bits, giving the starting address of a 32-bit block.
        const size_t blockSize = 32;
        const size_t mask = ~(blockSize - 1);
        const char* startAddr =
            reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(data) & mask);
        const size_t offset = data - startAddr;

        output << fmt::format(
            "BSONElement: bad type {0:d} @ {1:p} at offset {2:d} in block: ", *data, data, offset);

        for (size_t i = 0; i < blockSize; i++) {
            output << fmt::format("{0:#x} ", static_cast<uint8_t>(startAddr[i]));
        }
    }
    msgasserted(10320, output);
}
}  // namespace

int BSONElement::computeSize(int8_t type, const char* elem, int fieldNameSize, int bufSize) {
    enum SizeStyle : uint8_t {
        kFixed,         // Total size is a fixed amount + key length.
        kIntPlusFixed,  // Like Fixed, but also add in the int32 immediately following the key.
        kSpecial,       // Handled specially: RegEx, MinKey, MaxKey.
    };
    struct SizeInfo {
        uint8_t style : 2;
        uint8_t bytes : 6;  // Includes type byte. Excludes field name and variable lengths.
    };
    MONGO_STATIC_ASSERT(sizeof(SizeInfo) == 1);

    // This table should take 32 bytes. Align to that size to avoid splitting across cache lines
    // unnecessarily.
    static constexpr SizeInfo kSizeInfoTable alignas(32)[] = {
        {SizeStyle::kFixed, 1},          // EOO
        {SizeStyle::kFixed, 9},          // NumberDouble
        {SizeStyle::kIntPlusFixed, 5},   // String
        {SizeStyle::kIntPlusFixed, 1},   // Object
        {SizeStyle::kIntPlusFixed, 1},   // Array
        {SizeStyle::kIntPlusFixed, 6},   // BinData
        {SizeStyle::kFixed, 1},          // Undefined
        {SizeStyle::kFixed, 13},         // OID
        {SizeStyle::kFixed, 2},          // Bool
        {SizeStyle::kFixed, 9},          // Date
        {SizeStyle::kFixed, 1},          // Null
        {SizeStyle::kSpecial},           // Regex
        {SizeStyle::kIntPlusFixed, 17},  // DBRef
        {SizeStyle::kIntPlusFixed, 5},   // Code
        {SizeStyle::kIntPlusFixed, 5},   // Symbol
        {SizeStyle::kIntPlusFixed, 1},   // CodeWScope
        {SizeStyle::kFixed, 5},          // Int
        {SizeStyle::kFixed, 9},          // Timestamp
        {SizeStyle::kFixed, 9},          // Long
        {SizeStyle::kFixed, 17},         // Decimal
        {SizeStyle::kSpecial},           // reserved 20
        {SizeStyle::kSpecial},           // reserved 21
        {SizeStyle::kSpecial},           // reserved 22
        {SizeStyle::kSpecial},           // reserved 23
        {SizeStyle::kSpecial},           // reserved 24
        {SizeStyle::kSpecial},           // reserved 25
        {SizeStyle::kSpecial},           // reserved 26
        {SizeStyle::kSpecial},           // reserved 27
        {SizeStyle::kSpecial},           // reserved 28
        {SizeStyle::kSpecial},           // reserved 29
        {SizeStyle::kSpecial},           // reserved 30
        {SizeStyle::kSpecial},           // MinKey,  MaxKey
    };
    MONGO_STATIC_ASSERT(sizeof(kSizeInfoTable) == 32);

    // This function attempts to push complex handling of unlikely events out-of-line to ensure that
    // the common cases never need to spill any registers, which reduces the function call overhead.
    // Most invalid types have type != sizeInfoIndex and fall through to the cold path, as do RegEx,
    // MinKey, MaxKey and the remaining invalid types mapping to SizeStyle::kSpecial.
    int sizeInfoIndex = type % sizeof(kSizeInfoTable);
    const auto sizeInfo = kSizeInfoTable[sizeInfoIndex];
    if (MONGO_likely(type == sizeInfoIndex)) {
        if (sizeInfo.style == SizeStyle::kFixed)
            return sizeInfo.bytes + fieldNameSize;
        if (MONGO_likely(sizeInfo.style == SizeStyle::kIntPlusFixed))
            return sizeInfo.bytes + fieldNameSize +
                ConstDataView(elem + fieldNameSize + 1).read<LittleEndian<int32_t>>();
    }

    // The following code handles all special cases: MinKey, MaxKey, RegEx and invalid types.
    if (type == MaxKey || type == MinKey)
        return fieldNameSize + 1;
    if (type != BSONType::RegEx)
        msgAssertedBadType(elem);

    // RegEx is two c-strings back-to-back.
    const char* p = elem + fieldNameSize + 1;
    if (bufSize == 0) {
        size_t len1 = strlen(p);
        p = p + len1 + 1;
        size_t len2 = strlen(p);
        return (len1 + 1 + len2 + 1) + fieldNameSize + 1;
    } else {
        int searchSize = bufSize - fieldNameSize - 1;
        int len1 = strnlen(p, searchSize);
        if (len1 == searchSize)
            return -1;
        p = p + len1 + 1;
        searchSize -= len1 + 1;
        int len2 = strnlen(p, searchSize);
        if (len2 == searchSize)
            return -1;
        return (len1 + 1 + len2 + 1) + fieldNameSize + 1;
    }
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

    if (includeFieldName && type() != EOO)
        s << fieldName() << ": ";

    switch (type()) {
        case Object:
            return embeddedObject().toString(s, false, full, redactValues, depth + 1);
        case mongo::Array:
            return embeddedObject().toString(s, true, full, redactValues, depth + 1);
        default:
            break;
    }

    if (redactValues) {
        s << "\"###\"";
        return;
    }

    switch (type()) {
        case EOO:
            s << "EOO";
            break;
        case mongo::Date:
            s << "new Date(" << date().toMillisSinceEpoch() << ')';
            break;
        case RegEx: {
            s << "/" << regex() << '/';
            const char* p = regexFlags();
            if (p)
                s << p;
        } break;
        case NumberDouble:
            s.appendDoubleNice(number());
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
            s << _numberInt();
            break;
        case NumberDecimal:
            s << _numberDecimal().toString();
            break;
        case mongo::Bool:
            s << (boolean() ? "true" : "false");
            break;
        case Undefined:
            s << "undefined";
            break;
        case jstNULL:
            s << "null";
            break;
        case MaxKey:
            s << "MaxKey";
            break;
        case MinKey:
            s << "MinKey";
            break;
        case CodeWScope:
            s << "CodeWScope( " << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case Code:
            if (!full && valuestrsize() > 80) {
                s.write(valuestr(), 70);
                s << "...";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
            }
            break;
        case Symbol:
        case mongo::String:
            s << '"';
            if (!full && valuestrsize() > 160) {
                s.write(valuestr(), 150);
                s << "...\"";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
                s << '"';
            }
            break;
        case DBRef:
            s << "DBRef('" << valuestr() << "',";
            s << mongo::OID::from(valuestr() + valuestrsize()) << ')';
            break;
        case jstOID:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BinData: {
            int len;
            const char* data = binDataClean(len);
            // If the BinData is a correctly sized newUUID, display it as such.
            if (binDataType() == newUUID && len == 16) {
                using namespace fmt::literals;
                StringData sd(data, len);
                // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
                s << "UUID(\"{}-{}-{}-{}-{}\")"_format(hexblob::encodeLower(sd.substr(0, 4)),
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

        case bsonTimestamp: {
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
        case mongo::String:
        case Code:
            return valueStringData().toString();
        case CodeWScope:
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
    if (type() != mongo::String)
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
    if (type() != mongo::Array)
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
#if defined(_MSC_VER) && defined(_DEBUG)
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
#endif  // defined(_MSC_VER) && defined(_DEBUG)

}  // namespace mongo
