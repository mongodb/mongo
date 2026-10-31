// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement.h"

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
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace bson_details {
MONGO_COMPILER_NOINLINE void tassertedBSONOverread() {
    tasserted(ErrorCodes::BSONInternalError, "Invalid BSONElement detected");
}
}  // namespace bson_details
using namespace std::literals::string_view_literals;

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
            g.writeString(buffer, _valueStringDataUnchecked());
            break;
        case BSONType::symbol:
            g.writeSymbol(buffer, _valueStringDataUnchecked());
            break;
        case BSONType::numberLong:
            g.writeInt64(buffer, _numberLongUnchecked());
            break;
        case BSONType::numberInt:
            g.writeInt32(buffer, _numberIntUnchecked());
            break;
        case BSONType::numberDouble:
            g.writeDouble(buffer, _numberDoubleUnchecked());
            break;
        case BSONType::numberDecimal:
            g.writeDecimal128(buffer, _numberDecimalUnchecked());
            break;
        case BSONType::boolean:
            g.writeBool(buffer, _booleanUnchecked());
            break;
        case BSONType::null:
            g.writeNull(buffer);
            break;
        case BSONType::undefined:
            g.writeUndefined(buffer);
            break;
        case BSONType::object: {
            BSONObj truncated = _embeddedObjectUnchecked().jsonStringGenerator(
                g, pretty, false, buffer, writeLimit);
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
                _embeddedObjectUnchecked().jsonStringGenerator(g, pretty, true, buffer, writeLimit);
            if (!truncated.isEmpty()) {
                BSONObjBuilder builder;
                builder.append(fieldNameStringData(), truncated);
                return builder.obj();
            }
            // return to not check the write limit below, we're not in a leaf
            return truncated;
        }
        case BSONType::dbRef:
            g.writeDBRef(buffer, _dbrefNSStringViewUnchecked(), _dbrefOIDUnchecked());
            break;
        case BSONType::oid:
            g.writeOID(buffer, _oidUnchecked());
            break;
        case BSONType::binData: {
            ConstDataCursor reader(value());
            const int len = reader.readAndAdvance<LittleEndian<int>>();
            BinDataType type = static_cast<BinDataType>(reader.readAndAdvance<uint8_t>());
            g.writeBinData(buffer, std::string_view(reader.view(), len), type);
        }

        break;
        case BSONType::date:
            g.writeDate(buffer, _dateUnchecked());
            break;
        case BSONType::regEx: {
            std::string_view pattern(_regexUnchecked());
            g.writeRegex(buffer, pattern, std::string_view(pattern.data() + pattern.size() + 1));
        } break;
        case BSONType::codeWScope: {
            BSONObj scope = _codeWScopeObjectUnchecked();
            if (!scope.isEmpty()) {
                g.writeCodeWithScope(buffer, _codeWScopeCodeStringViewUnchecked(), scope);
                break;
            }
            g.writeCode(buffer, _codeWScopeCodeStringViewUnchecked());
            break;
        }
        case BSONType::code:
            g.writeCode(buffer, _valueStringDataUnchecked());
            break;
        case BSONType::timestamp:
            g.writeTimestamp(buffer, Timestamp(_timestampValueUnchecked()));
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
        truncationInfo.append("type"sv, typeName(type()));
        truncationInfo.append("size"sv, valuesize());
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

/** synthesize a 3-way comparison from `==` and `<`. */
int synthCmp(const auto& a, const auto& b) {
    return a == b ? 0 : a < b ? -1 : 1;
}

int compareStringValues(std::string_view leftStr, std::string_view rightStr) {
    // we use memcmp as we allow zeros in UTF8 strings
    int lsz = leftStr.size();
    int rsz = rightStr.size();
    int common = std::min(lsz, rsz);
    int res = memcmp(leftStr.data(), rightStr.data(), common);
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
    bson_details::tassertBSONOverread(l.canonicalType() == r.canonicalType());
    switch (l.type()) {
        case BSONType::eoo:
        case BSONType::undefined:  // EOO and Undefined are same canonicalType
        case BSONType::null:
        case BSONType::maxKey:
        case BSONType::minKey:
            return synthCmp(l.canonicalType(), r.canonicalType());
        case BSONType::boolean:
            return *l.value() - *r.value();
        case BSONType::timestamp:
            return synthCmp(l._timestampValueUnchecked(), r._timestampValueUnchecked());
        case BSONType::date:
            return synthCmp(l._dateUnchecked(), r._dateUnchecked());
        case BSONType::numberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (r.type()) {
                case BSONType::numberInt:
                    return compareInts(l._numberIntUnchecked(), r._numberIntUnchecked());
                case BSONType::numberLong:
                    return compareLongs(l._numberIntUnchecked(), r._numberLongUnchecked());
                case BSONType::numberDouble:
                    return compareDoubles(l._numberIntUnchecked(), r._numberDoubleUnchecked());
                case BSONType::numberDecimal:
                    return compareIntToDecimal(l._numberIntUnchecked(),
                                               r._numberDecimalUnchecked());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberLong: {
            switch (r.type()) {
                case BSONType::numberLong:
                    return compareLongs(l._numberLongUnchecked(), r._numberLongUnchecked());
                case BSONType::numberInt:
                    return compareLongs(l._numberLongUnchecked(), r._numberIntUnchecked());
                case BSONType::numberDouble:
                    return compareLongToDouble(l._numberLongUnchecked(),
                                               r._numberDoubleUnchecked());
                case BSONType::numberDecimal:
                    return compareLongToDecimal(l._numberLongUnchecked(),
                                                r._numberDecimalUnchecked());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberDouble: {
            switch (r.type()) {
                case BSONType::numberDouble:
                    return compareDoubles(l._numberDoubleUnchecked(), r._numberDoubleUnchecked());
                case BSONType::numberInt:
                    return compareDoubles(l._numberDoubleUnchecked(), r._numberIntUnchecked());
                case BSONType::numberLong:
                    return compareDoubleToLong(l._numberDoubleUnchecked(),
                                               r._numberLongUnchecked());
                case BSONType::numberDecimal:
                    return compareDoubleToDecimal(l._numberDoubleUnchecked(),
                                                  r._numberDecimalUnchecked());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::numberDecimal: {
            switch (r.type()) {
                case BSONType::numberDecimal:
                    return compareDecimals(l._numberDecimalUnchecked(),
                                           r._numberDecimalUnchecked());
                case BSONType::numberInt:
                    return compareDecimalToInt(l._numberDecimalUnchecked(),
                                               r._numberIntUnchecked());
                case BSONType::numberLong:
                    return compareDecimalToLong(l._numberDecimalUnchecked(),
                                                r._numberLongUnchecked());
                case BSONType::numberDouble:
                    return compareDecimalToDouble(l._numberDecimalUnchecked(),
                                                  r._numberDoubleUnchecked());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::oid:
            return memcmp(l.value(), r.value(), OID::kOIDSize);
        case BSONType::code:
            return compareStringValues(l._valueStringDataUnchecked(),
                                       r._valueStringDataUnchecked());
        case BSONType::symbol:
        case BSONType::string: {
            if (comparator) {
                return comparator->compare(l._valueStringDataUnchecked(),
                                           r._valueStringDataUnchecked());
            } else {
                return compareStringValues(l._valueStringDataUnchecked(),
                                           r._valueStringDataUnchecked());
            }
        }
        case BSONType::object:
        case BSONType::array: {
            return l._embeddedObjectUnchecked().woCompare(
                r._embeddedObjectUnchecked(),
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
            int lsz = l._objsizeUnchecked();  // our bin data size in bytes, not including the
                                              // subtype byte
            int rsz = r._objsizeUnchecked();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value() + 4, r.value() + 4, lsz + 1 /*+1 for subtype byte*/);
        }
        case BSONType::regEx: {
            int c = strcmp(l._regexUnchecked(), r._regexUnchecked());
            if (c)
                return c;
            return strcmp(l._regexFlagsUnchecked(), r._regexFlagsUnchecked());
        }
        case BSONType::codeWScope: {
            int cmp = l._codeWScopeCodeStringViewUnchecked().compare(
                r._codeWScopeCodeStringViewUnchecked());
            if (cmp)
                return cmp;

            // When comparing the scope object, we should consider field names. Special string
            // comparison semantics do not apply to strings nested inside the CodeWScope scope
            // object, so we do not pass through the string comparator.
            return l._codeWScopeObjectUnchecked().woCompare(
                r._codeWScopeObjectUnchecked(),
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
    for (auto element : _embeddedObjectUnchecked()) {
        auto fieldName = element.fieldNameStringData();
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid array index field name: \"{}\", expected \"{}\"",
                            fieldName,
                            static_cast<std::string_view>(counter)),
                fieldName == counter);
        ++counter;
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
        auto eDouble = _numberDoubleUnchecked();

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
        number = _numberDecimalUnchecked().toLongExact(&signalingFlags);
        if (signalingFlags != Decimal128::kNoFlag) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Cannot represent as a 64-bit integer: " << toString(true, true));
        }
    } else if (type() == BSONType::numberInt) {
        number = _numberIntUnchecked();
    } else {
        number = _numberLongUnchecked();
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

BSONObj BSONElement::_embeddedObjectUnchecked() const {
    return BSONObj(value(), BSONObj::LargeSizeTrait{});
}

BSONObj BSONElement::_codeWScopeObjectUnchecked() const {
    return BSONObj(value() + kCodeWScopeHeaderSize + _codeWScopeCodeLenUnchecked());
}

BSONObj BSONElement::embeddedObjectUserCheck() const {
    if (MONGO_likely(isABSONObj()))
        return _embeddedObjectUnchecked();
    uasserted(10065,
              str::stream() << "invalid parameter: expected an object (" << fieldName() << ")");
}

BSONObj BSONElement::embeddedObject() const {
    _checkObjectLike();
    return _embeddedObjectUnchecked();
}

BSONObj BSONElement::codeWScopeObject() const {
    _checkType(BSONType::codeWScope);
    return _codeWScopeObjectUnchecked();
}

// wrap this element up as a singleton object.
BSONObj BSONElement::wrap() const {
    BSONObjBuilder b(size() + 6);
    b.append(*this);
    return b.obj();
}

BSONObj BSONElement::wrap(std::string_view newName) const {
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

BSONElement BSONElement::operator[](std::string_view field) const {
    BSONObj o = Obj();
    return o[field];
}

int BSONElement::computeRegexSize(const char* elem,
                                  boost::optional<int> bufferSize,
                                  int fieldNameSize) {
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

    auto checkOffsetInBound = [&](int offset) {
        if (bufferSize) {
            bson_details::tassertBSONOverread(offset < *bufferSize);
        }
        return offset;
    };

    // RegEx is two c-strings back-to-back.
    const auto patternOffset = checkOffsetInBound(fieldNameSize + 1);
    const char* p = elem + patternOffset;
    size_t len1 = bufferSize ? strnlen(p, *bufferSize - patternOffset) : strlen(p);
    const auto optionsOffset = checkOffsetInBound(patternOffset + len1 + 1);
    p = elem + optionsOffset;
    size_t len2 = bufferSize ? strnlen(p, *bufferSize - optionsOffset) : strlen(p);
    return checkOffsetInBound(optionsOffset + len2 + 1);
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
            return _embeddedObjectUnchecked().toString(s, false, full, redactValues, depth + 1);
        case BSONType::array:
            return _embeddedObjectUnchecked().toString(s, true, full, redactValues, depth + 1);
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
            s << "new Date(" << _dateUnchecked().toMillisSinceEpoch() << ')';
            break;
        case BSONType::regEx: {
            s << "/" << _regexUnchecked() << '/';
            const char* p = _regexFlagsUnchecked();
            if (p)
                s << p;
        } break;
        case BSONType::numberDouble:
            s.appendDoubleNice(_numberDoubleUnchecked());
            break;
        case BSONType::numberLong:
            s << _numberLongUnchecked();
            break;
        case BSONType::numberInt:
            s << _numberIntUnchecked();
            break;
        case BSONType::numberDecimal:
            s << _numberDecimalUnchecked().toString();
            break;
        case BSONType::boolean:
            s << (_booleanUnchecked() ? "true" : "false");
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
            s << "CodeWScope( " << _codeWScopeCodeUnchecked() << ", "
              << _codeWScopeObjectUnchecked().toString() << ")";
            break;
        case BSONType::code:
            if (!full && _valuestrsizeUnchecked() > 80) {
                s.write(valuestr(), 70);
                s << "...";
            } else {
                s.write(valuestr(), _valuestrsizeUnchecked() - 1);
            }
            break;
        case BSONType::symbol:
        case BSONType::string:
            s << '"';
            if (!full && _valuestrsizeUnchecked() > 160) {
                s.write(valuestr(), 150);
                s << "...\"";
            } else {
                s.write(valuestr(), _valuestrsizeUnchecked() - 1);
                s << '"';
            }
            break;
        case BSONType::dbRef:
            s << "DBRef('" << _dbrefNSUnchecked() << "',";
            s << _dbrefOIDUnchecked() << ')';
            break;
        case BSONType::oid:
            s << "ObjectId('";
            s << _oidUnchecked() << "')";
            break;
        case BSONType::binData: {
            int len;
            const char* data = _binDataCleanUnchecked(len);
            // If the BinData is a correctly sized newUUID, display it as such.
            if (_binDataTypeUnchecked() == newUUID && len == 16) {
                std::string_view sd(data, len);
                // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
                s << fmt::format("UUID(\"{}-{}-{}-{}-{}\")",
                                 hexblob::encodeLower(sd.substr(0, 4)),
                                 hexblob::encodeLower(sd.substr(4, 2)),
                                 hexblob::encodeLower(sd.substr(6, 2)),
                                 hexblob::encodeLower(sd.substr(8, 2)),
                                 hexblob::encodeLower(sd.substr(10, 6)));
                break;
            }
            s << "BinData(" << _binDataTypeUnchecked() << ", ";
            if (!full && len > 80) {
                s << hexblob::encode(data, 70) << "...)";
            } else {
                s << hexblob::encode(data, std::max(len, 0)) << ")";
            }
        } break;

        case BSONType::timestamp: {
            // Convert from Milliseconds to Seconds for consistent Timestamp printing.
            auto secs = duration_cast<Seconds>(_timestampTimeUnchecked().toDurationSinceEpoch());
            s << "Timestamp(" << secs.count() << ", " << _timestampIncUnchecked() << ")";
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
            return std::string{_valueStringDataUnchecked()};
        case BSONType::codeWScope:
            return std::string{_codeWScopeCodeStringViewUnchecked()};
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
    *out = std::string{_valueStringDataUnchecked()};
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
    return _embeddedObjectUnchecked().coerceVector<std::string>(out);
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
