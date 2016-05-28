// key_string.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/key_string.h"

#include <cmath>

#include "mongo/base/data_view.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {
typedef KeyString::TypeBits TypeBits;

namespace CType {
// canonical types namespace. (would be enum class CType: uint8_t in C++11)
// Note 0-9 and 246-255 are disallowed and reserved for value encodings.
// For types that encode value information in the ctype byte, the value in this list is
// the "generic" one to be used to represent all values of that ctype, such as in the
// encoding of fields in Objects.
const uint8_t kMinKey = 10;
const uint8_t kUndefined = 15;
const uint8_t kNullish = 20;
const uint8_t kNumeric = 30;
const uint8_t kStringLike = 60;
const uint8_t kObject = 70;
const uint8_t kArray = 80;
const uint8_t kBinData = 90;
const uint8_t kOID = 100;
const uint8_t kBool = 110;
const uint8_t kDate = 120;
const uint8_t kTimestamp = 130;
const uint8_t kRegEx = 140;
const uint8_t kDBRef = 150;
const uint8_t kCode = 160;
const uint8_t kCodeWithScope = 170;
const uint8_t kMaxKey = 240;

// These are ordered by the numeric value of the values encoded in each format.
// Therefore each format can be considered independently without considering
// cross-format comparisons.
const uint8_t kNumericNaN = kNumeric + 0;
const uint8_t kNumericNegativeLargeMagnitude = kNumeric + 1;  // <= -2**63 including -Inf
const uint8_t kNumericNegative8ByteInt = kNumeric + 2;
const uint8_t kNumericNegative7ByteInt = kNumeric + 3;
const uint8_t kNumericNegative6ByteInt = kNumeric + 4;
const uint8_t kNumericNegative5ByteInt = kNumeric + 5;
const uint8_t kNumericNegative4ByteInt = kNumeric + 6;
const uint8_t kNumericNegative3ByteInt = kNumeric + 7;
const uint8_t kNumericNegative2ByteInt = kNumeric + 8;
const uint8_t kNumericNegative1ByteInt = kNumeric + 9;
const uint8_t kNumericNegativeSmallMagnitude = kNumeric + 10;  // between 0 and -1 exclusive
const uint8_t kNumericZero = kNumeric + 11;
const uint8_t kNumericPositiveSmallMagnitude = kNumeric + 12;  // between 0 and 1 exclusive
const uint8_t kNumericPositive1ByteInt = kNumeric + 13;
const uint8_t kNumericPositive2ByteInt = kNumeric + 14;
const uint8_t kNumericPositive3ByteInt = kNumeric + 15;
const uint8_t kNumericPositive4ByteInt = kNumeric + 16;
const uint8_t kNumericPositive5ByteInt = kNumeric + 17;
const uint8_t kNumericPositive6ByteInt = kNumeric + 18;
const uint8_t kNumericPositive7ByteInt = kNumeric + 19;
const uint8_t kNumericPositive8ByteInt = kNumeric + 20;
const uint8_t kNumericPositiveLargeMagnitude = kNumeric + 21;  // >= 2**63 including +Inf
static_assert(kNumericPositiveLargeMagnitude < kStringLike,
              "kNumericPositiveLargeMagnitude < kStringLike");

const uint8_t kBoolFalse = kBool + 0;
const uint8_t kBoolTrue = kBool + 1;
static_assert(kBoolTrue < kDate, "kBoolTrue < kDate");

size_t numBytesForInt(uint8_t ctype) {
    if (ctype >= kNumericPositive1ByteInt) {
        dassert(ctype <= kNumericPositive8ByteInt);
        return ctype - kNumericPositive1ByteInt + 1;
    }

    dassert(ctype <= kNumericNegative1ByteInt);
    dassert(ctype >= kNumericNegative8ByteInt);
    return kNumericNegative1ByteInt - ctype + 1;
}
}  // namespace CType

uint8_t bsonTypeToGenericKeyStringType(BSONType type) {
    switch (type) {
        case MinKey:
            return CType::kMinKey;

        case EOO:
        case jstNULL:
            return CType::kNullish;

        case Undefined:
            return CType::kUndefined;

        case NumberDouble:
        case NumberInt:
        case NumberLong:
            return CType::kNumeric;

        case mongo::String:
        case Symbol:
            return CType::kStringLike;

        case Object:
            return CType::kObject;
        case Array:
            return CType::kArray;
        case BinData:
            return CType::kBinData;
        case jstOID:
            return CType::kOID;
        case Bool:
            return CType::kBool;
        case Date:
            return CType::kDate;
        case bsonTimestamp:
            return CType::kTimestamp;
        case RegEx:
            return CType::kRegEx;
        case DBRef:
            return CType::kDBRef;

        case Code:
            return CType::kCode;
        case CodeWScope:
            return CType::kCodeWithScope;

        case MaxKey:
            return CType::kMaxKey;
        default:
            invariant(false);
    }
}

// Doubles smaller than this store only a single bit indicating a decimal continuation follows.
const double kTiniestDoubleWith2BitDCM = std::ldexp(1, -255);

// Amount to add to exponent of doubles tinier than kTiniestDoubleWith2BitDCM to avoid subnormals.
const int kTinyDoubleExponentShift = 256;

// Amount to multiply tiny doubles to perform a shift of the exponent by
// kSmallMagnitudeExponentShift.
const double kTinyDoubleExponentUpshiftFactor = std::ldexp(1, kTinyDoubleExponentShift);

// Amount to multiply scaled tiny doubles by to recover the unscaled value.
const double kTinyDoubleExponentDownshiftFactor = std::ldexp(1, -kTinyDoubleExponentShift);

// An underestimate of 2**256.
const Decimal128 kTinyDoubleExponentUpshiftFactorAsDecimal(std::ldexp(1, kTinyDoubleExponentShift),
                                                           Decimal128::kRoundTo34Digits,
                                                           Decimal128::kRoundTowardZero);

// An underestimate of 2**(-256).
const Decimal128 kTinyDoubleExponentDownshiftFactorAsDecimal(std::ldexp(1,
                                                                        -kTinyDoubleExponentShift),
                                                             Decimal128::kRoundTo34Digits,
                                                             Decimal128::kRoundTowardZero);

// First double that isn't an int64.
const double kMinLargeDouble = 1ULL << 63;

// Integers larger than this may not be representable as doubles.
const double kMaxIntForDouble = 1ULL << 53;

// Factors for scaling a double by powers of 256 to do a logical shift left of x bytes.
const double kPow256[] = {1.0,                                             // 2**0
                          1.0 * 256,                                       // 2**8
                          1.0 * 256 * 256,                                 // 2**16
                          1.0 * 256 * 256 * 256,                           // 2**24
                          1.0 * 256 * 256 * 256 * 256,                     // 2**32
                          1.0 * 256 * 256 * 256 * 256 * 256,               // 2**40
                          1.0 * 256 * 256 * 256 * 256 * 256 * 256,         // 2**48
                          1.0 * 256 * 256 * 256 * 256 * 256 * 256 * 256};  // 2**56
// Factors for scaling a double by negative powers of 256 to do a logical shift right of x bytes.
const double kInvPow256[] = {1.0,                                             // 2**0
                             1.0 / 256,                                       // 2**(-8)
                             1.0 / 256 / 256,                                 // 2**(-16)
                             1.0 / 256 / 256 / 256,                           // 2**(-24)
                             1.0 / 256 / 256 / 256 / 256,                     // 2**(-32)
                             1.0 / 256 / 256 / 256 / 256 / 256,               // 2**(-40)
                             1.0 / 256 / 256 / 256 / 256 / 256 / 256,         // 2**(-48)
                             1.0 / 256 / 256 / 256 / 256 / 256 / 256 / 256};  // 2**(-56)

const uint8_t kEnd = 0x4;

// These overlay with CType or kEnd bytes and therefor must be less/greater than all of
// them (and their inverses). They also can't equal 0 or 255 since that would collide with
// the encoding of NUL bytes in strings as "\x00\xff".
const uint8_t kLess = 1;
const uint8_t kGreater = 254;
}  // namespace

// some utility functions
namespace {
void memcpy_flipBits(void* dst, const void* src, size_t bytes) {
    const char* input = static_cast<const char*>(src);
    char* output = static_cast<char*>(dst);
    const char* const end = input + bytes;
    while (input != end) {
        *output++ = ~(*input++);
    }
}

template <typename T>
T readType(BufReader* reader, bool inverted) {
    // TODO for C++11 to static_assert that T is integral
    T t = ConstDataView(static_cast<const char*>(reader->skip(sizeof(T)))).read<T>();
    if (inverted)
        return ~t;
    return t;
}

StringData readCString(BufReader* reader) {
    const char* start = static_cast<const char*>(reader->pos());
    const char* end = static_cast<const char*>(memchr(start, 0x0, reader->remaining()));
    invariant(end);
    size_t actualBytes = end - start;
    reader->skip(1 + actualBytes);
    return StringData(start, actualBytes);
}

/**
 * scratch must be empty when passed in. It will be used if there is a NUL byte in the
 * output string. In that case the returned StringData will point into scratch, otherwise
 * it will point directly into the input buffer.
 */
StringData readCStringWithNuls(BufReader* reader, std::string* scratch) {
    const StringData initial = readCString(reader);
    if (reader->peek<unsigned char>() != 0xFF)
        return initial;  // Don't alloc or copy for simple case with no NUL bytes.

    scratch->append(initial.rawData(), initial.size());
    while (reader->peek<unsigned char>() == 0xFF) {
        // Each time we enter this loop it means we hit a NUL byte encoded as "\x00\xFF".
        *scratch += '\0';
        reader->skip(1);

        const StringData nextPart = readCString(reader);
        scratch->append(nextPart.rawData(), nextPart.size());
    }

    return *scratch;
}

string readInvertedCString(BufReader* reader) {
    const char* start = static_cast<const char*>(reader->pos());
    const char* end = static_cast<const char*>(memchr(start, 0xFF, reader->remaining()));
    invariant(end);
    size_t actualBytes = end - start;
    string s(start, actualBytes);
    for (size_t i = 0; i < s.size(); i++) {
        s[i] = ~s[i];
    }
    reader->skip(1 + actualBytes);
    return s;
}

string readInvertedCStringWithNuls(BufReader* reader) {
    std::string out;
    bool firstPass = true;
    do {
        if (firstPass) {
            firstPass = false;
        } else {
            // If this isn't our first pass through the loop it means we hit an NUL byte
            // encoded as "\xFF\00" in our inverted string.
            reader->skip(1);
            out += '\xFF';  // will be flipped to '\0' with rest of out before returning.
        }

        const char* start = static_cast<const char*>(reader->pos());
        const char* end = static_cast<const char*>(memchr(start, 0xFF, reader->remaining()));
        invariant(end);
        size_t actualBytes = end - start;

        out.append(start, actualBytes);
        reader->skip(1 + actualBytes);
    } while (reader->peek<unsigned char>() == 0x00);

    for (size_t i = 0; i < out.size(); i++) {
        out[i] = ~out[i];
    }

    return out;
}
}  // namespace

void KeyString::resetToKey(const BSONObj& obj, Ordering ord, RecordId recordId) {
    resetToEmpty();
    _appendAllElementsForIndexing(obj, ord, kInclusive);
    appendRecordId(recordId);
}

void KeyString::resetToKey(const BSONObj& obj, Ordering ord, Discriminator discriminator) {
    resetToEmpty();
    _appendAllElementsForIndexing(obj, ord, discriminator);
}

// ----------------------------------------------------------------------
// -----------   APPEND CODE  -------------------------------------------
// ----------------------------------------------------------------------

void KeyString::_appendAllElementsForIndexing(const BSONObj& obj,
                                              Ordering ord,
                                              Discriminator discriminator) {
    int elemCount = 0;
    BSONObjIterator it(obj);
    while (auto elem = it.next()) {
        const int elemIdx = elemCount++;
        const bool invert = (ord.get(elemIdx) == -1);

        _appendBsonValue(elem, invert, NULL);

        dassert(elem.fieldNameSize() < 3);  // fieldNameSize includes the NUL

        // IndexEntryComparison::makeQueryObject() encodes a discriminator in the first byte of
        // the field name. This discriminator overrides the passed in one. Normal elements only
        // have the NUL byte terminator. Entries stored in an index are not allowed to have a
        // discriminator.
        if (char ch = *elem.fieldName()) {
            // l for less / g for greater.
            invariant(ch == 'l' || ch == 'g');
            discriminator = ch == 'l' ? kExclusiveBefore : kExclusiveAfter;
            invariant(!it.more());
        }
    }

    // The discriminator forces this KeyString to compare Less/Greater than any KeyString with
    // the same prefix of keys. As an example, this can be used to land on the first key in the
    // index with the value "a" regardless of the RecordId. In compound indexes it can use a
    // prefix of the full key to ignore the later keys.
    switch (discriminator) {
        case kExclusiveBefore:
            _append(kLess, false);
            break;
        case kExclusiveAfter:
            _append(kGreater, false);
            break;
        case kInclusive:
            break;  // No discriminator byte.
    }

    // TODO consider omitting kEnd when using a discriminator byte. It is not a storage format
    // change since keystrings with discriminators are not allowed to be stored.
    _append(kEnd, false);
}

void KeyString::appendRecordId(RecordId loc) {
    // The RecordId encoding must be able to determine the full length starting from the last
    // byte, without knowing where the first byte is since it is stored at the end of a
    // KeyString, and we need to be able to read the RecordId without decoding the whole thing.
    //
    // This encoding places a number (N) between 0 and 7 in both the high 3 bits of the first
    // byte and the low 3 bits of the last byte. This is the number of bytes between the first
    // and last byte (ie total bytes is N + 2). The remaining bits of the first and last bytes
    // are combined with the bits of the in-between bytes to store the 64-bit RecordId in
    // big-endian order. This does not encode negative RecordIds to give maximum space to
    // positive RecordIds which are the only ones that are allowed to be stored in an index.

    int64_t raw = loc.repr();
    if (raw < 0) {
        // Note: we encode RecordId::min() and RecordId() the same which is ok, as they are
        // never stored so they will never be compared to each other.
        invariant(raw == RecordId::min().repr());
        raw = 0;
    }
    const uint64_t value = static_cast<uint64_t>(raw);
    const int bitsNeeded = 64 - countLeadingZeros64(raw);
    const int extraBytesNeeded =
        bitsNeeded <= 10 ? 0 : ((bitsNeeded - 10) + 7) / 8;  // ceil((bitsNeeded - 10) / 8)

    // extraBytesNeeded must fit in 3 bits.
    dassert(extraBytesNeeded >= 0 && extraBytesNeeded < 8);

    // firstByte combines highest 5 bits of value with extraBytesNeeded.
    const uint8_t firstByte =
        uint8_t((extraBytesNeeded << 5) | (value >> (5 + (extraBytesNeeded * 8))));
    // lastByte combines lowest 5 bits of value with extraBytesNeeded.
    const uint8_t lastByte = uint8_t((value << 3) | extraBytesNeeded);

    // RecordIds are never appended inverted.
    _append(firstByte, false);
    if (extraBytesNeeded) {
        const uint64_t extraBytes = endian::nativeToBig(value >> 5);
        // Only using the low-order extraBytesNeeded bytes of extraBytes.
        _appendBytes(reinterpret_cast<const char*>(&extraBytes) + sizeof(extraBytes) -
                         extraBytesNeeded,
                     extraBytesNeeded,
                     false);
    }
    _append(lastByte, false);
}

void KeyString::appendTypeBits(const TypeBits& typeBits) {
    // As an optimization, encode AllZeros as a single 0 byte.
    if (typeBits.isAllZeros()) {
        _append(uint8_t(0), false);
        return;
    }

    _appendBytes(typeBits.getBuffer(), typeBits.getSize(), false);
}

void KeyString::_appendBool(bool val, bool invert) {
    _append(val ? CType::kBoolTrue : CType::kBoolFalse, invert);
}

void KeyString::_appendDate(Date_t val, bool invert) {
    _append(CType::kDate, invert);
    // see: http://en.wikipedia.org/wiki/Offset_binary
    uint64_t encoded = static_cast<uint64_t>(val.asInt64());
    encoded ^= (1LL << 63);  // flip highest bit (equivalent to bias encoding)
    _append(endian::nativeToBig(encoded), invert);
}

void KeyString::_appendTimestamp(Timestamp val, bool invert) {
    _append(CType::kTimestamp, invert);
    _append(endian::nativeToBig(val.asLL()), invert);
}

void KeyString::_appendOID(OID val, bool invert) {
    _append(CType::kOID, invert);
    _appendBytes(val.view().view(), OID::kOIDSize, invert);
}

void KeyString::_appendString(StringData val, bool invert) {
    _typeBits.appendString();
    _append(CType::kStringLike, invert);
    _appendStringLike(val, invert);
}

void KeyString::_appendSymbol(StringData val, bool invert) {
    _typeBits.appendSymbol();
    _append(CType::kStringLike, invert);  // Symbols and Strings compare equally
    _appendStringLike(val, invert);
}

void KeyString::_appendCode(StringData val, bool invert) {
    _append(CType::kCode, invert);
    _appendStringLike(val, invert);
}

void KeyString::_appendCodeWString(const BSONCodeWScope& val, bool invert) {
    _append(CType::kCodeWithScope, invert);
    _appendStringLike(val.code, invert);
    _appendBson(val.scope, invert);
}

void KeyString::_appendBinData(const BSONBinData& val, bool invert) {
    _append(CType::kBinData, invert);
    if (val.length < 0xff) {
        // size fits in one byte so use one byte to encode.
        _append(uint8_t(val.length), invert);
    } else {
        // Encode 0xff prefix to indicate that the size takes 4 bytes.
        _append(uint8_t(0xff), invert);
        _append(endian::nativeToBig(int32_t(val.length)), invert);
    }
    _append(uint8_t(val.type), invert);
    _appendBytes(val.data, val.length, invert);
}

void KeyString::_appendRegex(const BSONRegEx& val, bool invert) {
    _append(CType::kRegEx, invert);
    // note: NULL is not allowed in pattern or flags
    _appendBytes(val.pattern.rawData(), val.pattern.size(), invert);
    _append(int8_t(0), invert);
    _appendBytes(val.flags.rawData(), val.flags.size(), invert);
    _append(int8_t(0), invert);
}

void KeyString::_appendDBRef(const BSONDBRef& val, bool invert) {
    _append(CType::kDBRef, invert);
    _append(endian::nativeToBig(int32_t(val.ns.size())), invert);
    _appendBytes(val.ns.rawData(), val.ns.size(), invert);
    _appendBytes(val.oid.view().view(), OID::kOIDSize, invert);
}

void KeyString::_appendArray(const BSONArray& val, bool invert) {
    _append(CType::kArray, invert);
    BSONForEach(elem, val) {
        // No generic ctype byte needed here since no name is encoded.
        _appendBsonValue(elem, invert, NULL);
    }
    _append(int8_t(0), invert);
}

void KeyString::_appendObject(const BSONObj& val, bool invert) {
    _append(CType::kObject, invert);
    _appendBson(val, invert);
}

void KeyString::_appendNumberDouble(const double num, bool invert) {
    if (num == 0.0 && std::signbit(num))
        _typeBits.appendZero(TypeBits::kNegativeDoubleZero);
    else
        _typeBits.appendNumberDouble();

    _appendDoubleWithoutTypeBits(num, kDCMEqualToDouble, invert);
}

void KeyString::_appendDoubleWithoutTypeBits(const double num,
                                             DecimalContinuationMarker dcm,
                                             bool invert) {
    const bool isNegative = num < 0.0;
    const double magnitude = isNegative ? -num : num;

    // Tests are structured such that NaNs and infinities fall through correctly.
    if (!(magnitude >= 1.0)) {
        if (magnitude > 0.0) {
            // This includes subnormal numbers.
            _appendSmallDouble(num, dcm, invert);
        } else if (num == 0.0) {
            // We are collapsing -0.0 and 0.0 to the same value here, the type bits disambiguate.
            _append(CType::kNumericZero, invert);
        } else {
            invariant(std::isnan(num));
            _append(CType::kNumericNaN, invert);
        }
        return;
    }

    if (magnitude < kMinLargeDouble) {
        uint64_t integerPart = static_cast<uint64_t>(magnitude);
        if (static_cast<double>(integerPart) == magnitude && dcm == kDCMEqualToDouble) {
            // No fractional part
            _appendPreshiftedIntegerPortion(integerPart << 1, isNegative, invert);
            return;
        }

        if (version == Version::V0) {
            invariant(dcm == kDCMEqualToDouble);
            // There is a fractional part.
            _appendPreshiftedIntegerPortion((integerPart << 1) | 1, isNegative, invert);

            // Append the bytes of the mantissa that include fractional bits.
            const size_t fractionalBits = 53 - (64 - countLeadingZeros64(integerPart));
            const size_t fractionalBytes = (fractionalBits + 7) / 8;
            dassert(fractionalBytes > 0);
            uint64_t mantissa;
            memcpy(&mantissa, &num, sizeof(mantissa));
            mantissa &= ~(uint64_t(-1) << fractionalBits);  // set non-fractional bits to 0;

            mantissa = endian::nativeToBig(mantissa);

            const void* firstUsedByte =
                reinterpret_cast<const char*>((&mantissa) + 1) - fractionalBytes;
            _appendBytes(firstUsedByte, fractionalBytes, isNegative ? !invert : invert);
        } else {
            const size_t fractionalBytes = countLeadingZeros64(integerPart << 1) / 8;
            const auto ctype = isNegative ? CType::kNumericNegative8ByteInt + fractionalBytes
                                          : CType::kNumericPositive8ByteInt - fractionalBytes;
            _append(static_cast<uint8_t>(ctype), invert);

            // Multiplying the double by 256 to the power X is logically equivalent to shifting the
            // fraction left by X bytes.
            uint64_t encoding = static_cast<uint64_t>(magnitude * kPow256[fractionalBytes]);
            dassert(encoding == magnitude * kPow256[fractionalBytes]);

            // Merge in the bit indicating the value has a fractional part by doubling the integer
            // part and adding 1. This leaves encoding with the high 8-fractionalBytes bytes in the
            // same form they'd have with _appendPreshiftedIntegerPortion(). The remaining low bytes
            // are the fractional bytes left-shifted by 2 bits to make room for the DCM.
            encoding += (integerPart + 1) << (fractionalBytes * 8);
            invariant((encoding & 0x3ULL) == 0);
            encoding |= dcm;
            encoding = endian::nativeToBig(encoding);
            _append(encoding, isNegative ? !invert : invert);
        }
    } else {
        _appendLargeDouble(num, dcm, invert);
    }
}

void KeyString::_appendNumberLong(const long long num, bool invert) {
    _typeBits.appendNumberLong();
    _appendInteger(num, invert);
}

void KeyString::_appendNumberInt(const int num, bool invert) {
    _typeBits.appendNumberInt();
    _appendInteger(num, invert);
}

void KeyString::_appendNumberDecimal(const Decimal128 dec, bool invert) {
    bool isNegative = dec.isNegative();
    if (dec.isZero()) {
        uint32_t zeroExp = dec.getBiasedExponent();
        if (isNegative)
            zeroExp += Decimal128::kMaxBiasedExponent + 1;

        _typeBits.appendDecimalZero(zeroExp);
        _append(CType::kNumericZero, invert);
        return;
    }

    if (dec.isNaN()) {
        _append(CType::kNumericNaN, invert);
        _typeBits.appendNumberDecimal();
        return;
    }

    if (dec.isInfinite()) {
        _append(isNegative ? CType::kNumericNegativeLargeMagnitude
                           : CType::kNumericPositiveLargeMagnitude,
                invert);
        const uint64_t infinity = ~0ULL;
        _append(infinity, isNegative ? !invert : invert);
        _typeBits.appendNumberDecimal();
        return;
    }

    const uint32_t biasedExponent = dec.getBiasedExponent();
    dassert(biasedExponent <= Decimal128::kInfinityExponent);
    _typeBits.appendNumberDecimal();
    _typeBits.appendDecimalExponent(biasedExponent & TypeBits::kStoredDecimalExponentMask);

    uint32_t signalingFlags = Decimal128::kNoFlag;
    double bin = dec.toDouble(&signalingFlags, Decimal128::kRoundTowardZero);

    // Easy case: the decimal actually is a double. True for many integers, fractions like 1.5, etc.
    if (!Decimal128::hasFlag(signalingFlags, Decimal128::kInexact) &&
        !Decimal128::hasFlag(signalingFlags, Decimal128::kOverflow)) {
        _appendDoubleWithoutTypeBits(bin, kDCMEqualToDouble, invert);
        return;
    }

    // Values smaller than the double normalized range need special handling: a regular double
    // wouldn't give 15 digits, if any at all.
    if (std::abs(bin) < std::numeric_limits<double>::min()) {
        _appendTinyDecimalWithoutTypeBits(dec, bin, invert);
        return;
    }

    // Huge finite values are encoded directly. Because the value is not exact, and truncates
    // to the maximum double, the original decimal was outside of the range of finite doubles.
    // Because all decimals larger than the max finite double round down to that value, strict
    // less-than would be incorrect.
    if (std::abs(bin) >= std::numeric_limits<double>::max()) {
        _appendHugeDecimalWithoutTypeBits(dec, invert);
        return;
    }

    const auto roundAwayFromZero =
        isNegative ? Decimal128::kRoundTowardNegative : Decimal128::kRoundTowardPositive;
    const uint64_t k1E15 = 1E15;  // Exact in both double and int64_t.

    // If the conditions below fall through, a decimal continuation is needed to represent the
    // difference between the stored value and the actual decimal. All paths that fall through
    // must set 'storedValue', overwriting the NaN.
    Decimal128 storedValue = Decimal128::kPositiveNaN;

    // For doubles in this range, 'bin' may have lost precision in the integer part, which would
    // lead to miscompares with integers. So, instead handle explicitly.
    if ((bin <= -kMaxIntForDouble || bin >= kMaxIntForDouble) && bin > -kMinLargeDouble &&
        bin < kMinLargeDouble) {
        uint32_t signalingFlags = Decimal128::kNoFlag;
        Decimal128 truncated = dec.quantize(
            Decimal128::kNormalizedZero, &signalingFlags, Decimal128::kRoundTowardZero);
        dassert(truncated.getBiasedExponent() == Decimal128::kExponentBias);
        dassert(truncated.getCoefficientHigh() == 0 &&
                truncated.getCoefficientLow() < (1ULL << 63));
        int64_t integerPart = truncated.getCoefficientLow();
        bool hasFraction = Decimal128::hasFlag(signalingFlags, Decimal128::kInexact);
        bool isNegative = truncated.isNegative();
        bool has8bytes = integerPart >= (1LL << 55);
        uint64_t preshifted = integerPart << 1;
        _appendPreshiftedIntegerPortion(preshifted | hasFraction, isNegative, invert);
        if (!hasFraction)
            return;
        if (!has8bytes) {
            // A Fraction byte follows, but the leading 7 bytes already encode 53 bits of the
            // coefficient, so just store the DCM.
            uint8_t dcm = kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;
            _append(dcm, isNegative ? !invert : invert);
        }

        storedValue = Decimal128(isNegative, Decimal128::kExponentBias, 0, integerPart);

        // Common case: the coefficient less than 1E15, so at most 15 digits, and the number is
        // in the normal range of double, so the decimal can be represented with at least 15 digits
        // of precision by the double 'bin'
    } else if (dec.getCoefficientHigh() == 0 && dec.getCoefficientLow() < k1E15) {
        dassert(Decimal128(
                    std::abs(bin), Decimal128::kRoundTo15Digits, Decimal128::kRoundTowardPositive)
                    .isEqual(dec.toAbs()));
        _appendDoubleWithoutTypeBits(bin, kDCMEqualToDoubleRoundedUpTo15Digits, invert);
        return;
    } else {
        // The coefficient has more digits, but may still be 15 digits after removing trailing
        // zeros.
        Decimal128 decFromBin = Decimal128(bin, Decimal128::kRoundTo15Digits, roundAwayFromZero);
        if (decFromBin.isEqual(dec)) {
            _appendDoubleWithoutTypeBits(bin, kDCMEqualToDoubleRoundedUpTo15Digits, invert);
            return;
        }
        // Harder cases: decimal continuation is needed.
        // First store the double and kind of continuation needed.
        DecimalContinuationMarker dcm = dec.isLess(decFromBin) != isNegative
            ? kDCMHasContinuationLessThanDoubleRoundedUpTo15Digits
            : kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;
        _appendDoubleWithoutTypeBits(bin, dcm, invert);

        // Note that 'dec' and 'bin' can be negative.
        storedValue = Decimal128(bin, Decimal128::kRoundTo34Digits, roundAwayFromZero);
    }
    invariant(!storedValue.isNaN());        // Should have been set explicitly.
    storedValue = storedValue.normalize();  // Normalize to 34 digits to fix decDiff exponent.

    Decimal128 decDiff = dec.subtract(storedValue);
    invariant(decDiff.isNegative() == dec.isNegative() || decDiff.isZero());
    invariant(decDiff.getBiasedExponent() == storedValue.getBiasedExponent());
    invariant(decDiff.getCoefficientHigh() == 0);

    // Now we know that we can recover the original decimal value (but not its precision, which is
    // given by the type bits) from the binary double plus the decimal continuation.
    uint64_t decimalContinuation = decDiff.getCoefficientLow();
    dassert(
        storedValue
            .add(Decimal128(isNegative, storedValue.getBiasedExponent(), 0, decimalContinuation))
            .isEqual(dec));
    decimalContinuation = endian::nativeToBig(decimalContinuation);
    _append(decimalContinuation, isNegative ? !invert : invert);
}

void KeyString::_appendBsonValue(const BSONElement& elem, bool invert, const StringData* name) {
    if (name) {
        _appendBytes(name->rawData(), name->size() + 1, invert);  // + 1 for NUL
    }

    switch (elem.type()) {
        case MinKey:
        case MaxKey:
        case EOO:
        case Undefined:
        case jstNULL:
            _append(bsonTypeToGenericKeyStringType(elem.type()), invert);
            break;

        case NumberDouble:
            _appendNumberDouble(elem._numberDouble(), invert);
            break;
        case String:
            _appendString(elem.valueStringData(), invert);
            break;
        case Object:
            _appendObject(elem.Obj(), invert);
            break;
        case Array:
            _appendArray(BSONArray(elem.Obj()), invert);
            break;
        case BinData: {
            int len;
            const char* data = elem.binData(len);
            _appendBinData(BSONBinData(data, len, elem.binDataType()), invert);
            break;
        }

        case jstOID:
            _appendOID(elem.__oid(), invert);
            break;
        case Bool:
            _appendBool(elem.boolean(), invert);
            break;
        case Date:
            _appendDate(elem.date(), invert);
            break;

        case RegEx:
            _appendRegex(BSONRegEx(elem.regex(), elem.regexFlags()), invert);
            break;
        case DBRef:
            _appendDBRef(BSONDBRef(elem.dbrefNS(), elem.dbrefOID()), invert);
            break;
        case Symbol:
            _appendSymbol(elem.valueStringData(), invert);
            break;
        case Code:
            _appendCode(elem.valueStringData(), invert);
            break;
        case CodeWScope: {
            _appendCodeWString(
                BSONCodeWScope(StringData(elem.codeWScopeCode(), elem.codeWScopeCodeLen() - 1),
                               BSONObj(elem.codeWScopeScopeData())),
                invert);
            break;
        }
        case NumberInt:
            _appendNumberInt(elem._numberInt(), invert);
            break;
        case bsonTimestamp:
            _appendTimestamp(elem.timestamp(), invert);
            break;
        case NumberLong:
            _appendNumberLong(elem._numberLong(), invert);
            break;
        case NumberDecimal:
            uassert(ErrorCodes::UnsupportedFormat,
                    "Index version does not support NumberDecimal",
                    version >= Version::V1);
            _appendNumberDecimal(elem._numberDecimal(), invert);
            break;

        default:
            invariant(false);
    }
}


/// -- lowest level

void KeyString::_appendStringLike(StringData str, bool invert) {
    while (true) {
        size_t firstNul = strnlen(str.rawData(), str.size());
        // No NULs in string.
        _appendBytes(str.rawData(), firstNul, invert);
        if (firstNul == str.size() || firstNul == std::string::npos) {
            _append(int8_t(0), invert);
            break;
        }

        // replace "\x00" with "\x00\xFF"
        _appendBytes("\x00\xFF", 2, invert);
        str = str.substr(firstNul + 1);  // skip over the NUL byte
    }
}

void KeyString::_appendBson(const BSONObj& obj, bool invert) {
    BSONForEach(elem, obj) {
        // Force the order to be based on (ctype, name, value).
        _append(bsonTypeToGenericKeyStringType(elem.type()), invert);
        StringData name = elem.fieldNameStringData();
        _appendBsonValue(elem, invert, &name);
    }
    _append(int8_t(0), invert);
}

void KeyString::_appendSmallDouble(double value, DecimalContinuationMarker dcm, bool invert) {
    bool isNegative = value < 0;
    double magnitude = isNegative ? -value : value;
    dassert(!std::isnan(value) && value != 0 && magnitude < 1);

    _append(isNegative ? CType::kNumericNegativeSmallMagnitude
                       : CType::kNumericPositiveSmallMagnitude,
            invert);

    uint64_t encoded;

    if (version == KeyString::Version::V0) {
        // Not using magnitude to preserve sign bit in V0
        memcpy(&encoded, &value, sizeof(encoded));
    } else if (magnitude >= kTiniestDoubleWith2BitDCM) {
        // Values in the range [2**(-255), 1) get the prefix 0b11
        memcpy(&encoded, &magnitude, sizeof(encoded));
        dassert((encoded & (0x3ULL << 62)) == 0);
        encoded <<= 2;
        encoded |= dcm;
        dassert(encoded >> 62 == 0x3);
    } else {
        // Values in the range [numeric_limits<double>::denorm_min(), 2**(-255)) get the prefixes
        // 0b01 or 0b10. The 0b00 prefix is used by _appendHugeDecimalWithoutTypeBits for decimals
        // smaller than that.
        magnitude *= kTinyDoubleExponentUpshiftFactor;
        memcpy(&encoded, &magnitude, sizeof(encoded));
        encoded <<= 1;
        encoded |= (dcm != kDCMEqualToDouble);
        // Change the two most significant bits from 0b00 or 0b01 to 0b01 or 0b10.
        encoded += (1ULL << 62);
        dassert(encoded >> 62 == 0x1 || encoded >> 62 == 0x2);
    }

    _append(endian::nativeToBig(encoded), isNegative ? !invert : invert);
}

void KeyString::_appendLargeDouble(double value, DecimalContinuationMarker dcm, bool invert) {
    dassert(!std::isnan(value));
    dassert(value != 0.0);

    _append(value > 0 ? CType::kNumericPositiveLargeMagnitude
                      : CType::kNumericNegativeLargeMagnitude,
            invert);

    uint64_t encoded;
    memcpy(&encoded, &value, sizeof(encoded));

    if (version != Version::V0) {
        if (std::isfinite(value)) {
            encoded <<= 1;
            encoded &= ~(1ULL << 63);
            encoded |= (dcm != kDCMEqualToDouble);
        } else {
            encoded = ~0ULL;  // infinity
        }
    }
    encoded = endian::nativeToBig(encoded);
    _append(encoded, value > 0 ? invert : !invert);
}

void KeyString::_appendTinyDecimalWithoutTypeBits(const Decimal128 dec,
                                                  const double bin,
                                                  bool invert) {
    // This function is only for 'dec' that doesn't exactly equal a double, but rounds to 'bin'
    dassert(bin == dec.toDouble(Decimal128::kRoundTowardZero));
    dassert(std::abs(bin) < DBL_MIN);
    const bool isNegative = dec.isNegative();
    Decimal128 magnitude = isNegative ? dec.negate() : dec;

    _append(isNegative ? CType::kNumericNegativeSmallMagnitude
                       : CType::kNumericPositiveSmallMagnitude,
            invert);

    // For decimals smaller than the smallest subnormal double, just store the decimal number
    if (bin == 0.0) {
        Decimal128 normalized = magnitude.normalize();
        uint64_t hi = normalized.getValue().high64;
        uint64_t lo = normalized.getValue().low64;
        invariant((hi & (0x3ULL << 62)) == 0);
        _append(endian::nativeToBig(hi), isNegative ? !invert : invert);
        _append(endian::nativeToBig(lo), isNegative ? !invert : invert);
        return;
    }
    // Encode decimal in subnormal double range by scaling in the decimal domain. Round down at
    // each step, but ensure not to get below the subnormal double. This will ensure that
    // 'scaledBin' is monotonically increasing and will only be off by at most a few units in the
    // last place, so the decimal continuation will stay in range.
    Decimal128 scaledDec =
        magnitude.multiply(kTinyDoubleExponentUpshiftFactorAsDecimal, Decimal128::kRoundTowardZero);
    double scaledBin = scaledDec.toDouble(Decimal128::kRoundTowardZero);

    // Here we know that scaledBin contains the first 15 significant digits of scaled dec, and
    // sorts correctly with scaled double.
    scaledBin = std::max(scaledBin, std::abs(bin) * kTinyDoubleExponentUpshiftFactor);
    uint64_t encoded;
    memcpy(&encoded, &scaledBin, sizeof(encoded));
    encoded <<= 1;
    encoded |= 1;  // Even if decDiff.isZero() we aren't exactly equal
    encoded += (1ULL << 62);
    dassert(encoded >> 62 == 0x1);
    _append(endian::nativeToBig(encoded), isNegative ? !invert : invert);

    Decimal128 storedVal(scaledBin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardPositive);
    storedVal =
        storedVal
            .multiply(kTinyDoubleExponentDownshiftFactorAsDecimal, Decimal128::kRoundTowardZero)
            .add(Decimal128::kLargestNegativeExponentZero);
    dassert(storedVal.isLess(magnitude));
    Decimal128 decDiff = magnitude.subtract(storedVal);
    dassert(decDiff.getBiasedExponent() == storedVal.getBiasedExponent() || decDiff.isZero());
    dassert(decDiff.getCoefficientHigh() == 0 && !decDiff.isNegative());
    uint64_t continuation = decDiff.getCoefficientLow();
    _append(endian::nativeToBig(continuation), isNegative ? !invert : invert);
}


void KeyString::_appendHugeDecimalWithoutTypeBits(const Decimal128 dec, bool invert) {
    // To allow us to use CType::kNumericNegativeLargeMagnitude we need to fit between the highest
    // finite double and the representation of +/-Inf. We do this by forcing the high bit to 1
    // (large doubles always have 0) and never encoding ~0 here.

    const bool isNegative = dec.isNegative();
    Decimal128 normalizedMagnitude = (isNegative ? dec.negate() : dec).normalize();
    uint64_t hi = normalizedMagnitude.getValue().high64;
    uint64_t lo = normalizedMagnitude.getValue().low64;
    dassert(hi < (1ULL << 63));
    hi |= (1ULL << 63);
    _append(isNegative ? CType::kNumericNegativeLargeMagnitude
                       : CType::kNumericPositiveLargeMagnitude,
            invert);
    _append(endian::nativeToBig(hi), isNegative ? !invert : invert);
    _append(endian::nativeToBig(lo), isNegative ? !invert : invert);
}

// Handles NumberLong and NumberInt which are encoded identically except for the TypeBits.
void KeyString::_appendInteger(const long long num, bool invert) {
    if (num == std::numeric_limits<long long>::min()) {
        // -2**63 is exactly representable as a double and not as a positive int64.
        // Therefore we encode it as a double.
        dassert(-double(num) == kMinLargeDouble);
        _appendLargeDouble(static_cast<double>(num), kDCMEqualToDouble, invert);
        return;
    }

    if (num == 0) {
        _append(CType::kNumericZero, invert);
        return;
    }

    const bool isNegative = num < 0;
    const uint64_t magnitude = isNegative ? -num : num;
    _appendPreshiftedIntegerPortion(magnitude << 1, isNegative, invert);
}

void KeyString::_appendPreshiftedIntegerPortion(uint64_t value, bool isNegative, bool invert) {
    dassert(value != 0ULL);
    dassert(value != 1ULL);

    const size_t bytesNeeded = (64 - countLeadingZeros64(value) + 7) / 8;

    // Append the low bytes of value in big endian order.
    value = endian::nativeToBig(value);
    const void* firstUsedByte = reinterpret_cast<const char*>((&value) + 1) - bytesNeeded;

    if (isNegative) {
        _append(uint8_t(CType::kNumericNegative1ByteInt - (bytesNeeded - 1)), invert);
        _appendBytes(firstUsedByte, bytesNeeded, !invert);
    } else {
        _append(uint8_t(CType::kNumericPositive1ByteInt + (bytesNeeded - 1)), invert);
        _appendBytes(firstUsedByte, bytesNeeded, invert);
    }
}

template <typename T>
void KeyString::_append(const T& thing, bool invert) {
    _appendBytes(&thing, sizeof(thing), invert);
}

void KeyString::_appendBytes(const void* source, size_t bytes, bool invert) {
    char* const base = _buffer.skip(bytes);

    if (invert) {
        memcpy_flipBits(base, source, bytes);
    } else {
        memcpy(base, source, bytes);
    }
}


// ----------------------------------------------------------------------
// ----------- DECODING CODE --------------------------------------------
// ----------------------------------------------------------------------

namespace {
void toBsonValue(uint8_t ctype,
                 BufReader* reader,
                 TypeBits::Reader* typeBits,
                 bool inverted,
                 KeyString::Version version,
                 BSONObjBuilderValueStream* stream);

void toBson(BufReader* reader,
            TypeBits::Reader* typeBits,
            bool inverted,
            KeyString::Version version,
            BSONObjBuilder* builder) {
    while (readType<uint8_t>(reader, inverted) != 0) {
        if (inverted) {
            std::string name = readInvertedCString(reader);
            BSONObjBuilderValueStream& stream = *builder << name;
            toBsonValue(
                readType<uint8_t>(reader, inverted), reader, typeBits, inverted, version, &stream);
        } else {
            StringData name = readCString(reader);
            BSONObjBuilderValueStream& stream = *builder << name;
            toBsonValue(
                readType<uint8_t>(reader, inverted), reader, typeBits, inverted, version, &stream);
        }
    }
}

/**
 * Helper function to read the least significant type bits for 'num' and return a value that
 * is numerically equal to 'num', but has its exponent adjusted to match the stored exponent bits.
 */
Decimal128 adjustDecimalExponent(TypeBits::Reader* typeBits, Decimal128 num);

/**
 * Helper function that takes a 'num' with 15 decimal digits of precision, normalizes it to 34
 * digits and reads a 64-bit (19-digit) continuation to obtain the full 34-bit value.
 */
Decimal128 readDecimalContinuation(BufReader* reader, bool inverted, Decimal128 num) {
    uint32_t flags = Decimal128::kNoFlag;
    uint64_t continuation = endian::bigToNative(readType<uint64_t>(reader, inverted));
    num = num.normalize();
    num = num.add(Decimal128(num.isNegative(), num.getBiasedExponent(), 0, continuation), &flags);
    invariant(!(Decimal128::hasFlag(flags, Decimal128::kInexact)));
    return num;
}

void toBsonValue(uint8_t ctype,
                 BufReader* reader,
                 TypeBits::Reader* typeBits,
                 bool inverted,
                 KeyString::Version version,
                 BSONObjBuilderValueStream* stream) {
    // This is only used by the kNumeric.*ByteInt types, but needs to be declared up here
    // since it is used across a fallthrough.
    bool isNegative = false;

    switch (ctype) {
        case CType::kMinKey:
            *stream << MINKEY;
            break;
        case CType::kMaxKey:
            *stream << MAXKEY;
            break;
        case CType::kNullish:
            *stream << BSONNULL;
            break;
        case CType::kUndefined:
            *stream << BSONUndefined;
            break;

        case CType::kBoolTrue:
            *stream << true;
            break;
        case CType::kBoolFalse:
            *stream << false;
            break;

        case CType::kDate:
            *stream << Date_t::fromMillisSinceEpoch(
                endian::bigToNative(readType<uint64_t>(reader, inverted)) ^ (1LL << 63));
            break;

        case CType::kTimestamp:
            *stream << Timestamp(endian::bigToNative(readType<uint64_t>(reader, inverted)));
            break;

        case CType::kOID:
            if (inverted) {
                char buf[OID::kOIDSize];
                memcpy_flipBits(buf, reader->skip(OID::kOIDSize), OID::kOIDSize);
                *stream << OID::from(buf);
            } else {
                *stream << OID::from(reader->skip(OID::kOIDSize));
            }
            break;

        case CType::kStringLike: {
            const uint8_t originalType = typeBits->readStringLike();
            if (inverted) {
                if (originalType == TypeBits::kString) {
                    *stream << readInvertedCStringWithNuls(reader);
                } else {
                    dassert(originalType == TypeBits::kSymbol);
                    *stream << BSONSymbol(readInvertedCStringWithNuls(reader));
                }

            } else {
                std::string scratch;
                if (originalType == TypeBits::kString) {
                    *stream << readCStringWithNuls(reader, &scratch);
                } else {
                    dassert(originalType == TypeBits::kSymbol);
                    *stream << BSONSymbol(readCStringWithNuls(reader, &scratch));
                }
            }
            break;
        }

        case CType::kCode: {
            if (inverted) {
                *stream << BSONCode(readInvertedCStringWithNuls(reader));
            } else {
                std::string scratch;
                *stream << BSONCode(readCStringWithNuls(reader, &scratch));
            }
            break;
        }

        case CType::kCodeWithScope: {
            std::string scratch;
            StringData code;  // will point to either scratch or the raw encoded bytes.
            if (inverted) {
                scratch = readInvertedCStringWithNuls(reader);
                code = scratch;
            } else {
                code = readCStringWithNuls(reader, &scratch);
            }
            // Not going to optimize CodeWScope.
            BSONObjBuilder scope;
            toBson(reader, typeBits, inverted, version, &scope);
            *stream << BSONCodeWScope(code, scope.done());
            break;
        }

        case CType::kBinData: {
            size_t size = readType<uint8_t>(reader, inverted);
            if (size == 0xff) {
                // size was stored in 4 bytes.
                size = endian::bigToNative(readType<uint32_t>(reader, inverted));
            }
            BinDataType subType = BinDataType(readType<uint8_t>(reader, inverted));
            const void* ptr = reader->skip(size);
            if (!inverted) {
                *stream << BSONBinData(ptr, size, subType);
            } else {
                std::unique_ptr<char[]> flipped(new char[size]);
                memcpy_flipBits(flipped.get(), ptr, size);
                *stream << BSONBinData(flipped.get(), size, subType);
            }
            break;
        }

        case CType::kRegEx: {
            if (inverted) {
                string pattern = readInvertedCString(reader);
                string flags = readInvertedCString(reader);
                *stream << BSONRegEx(pattern, flags);
            } else {
                StringData pattern = readCString(reader);
                StringData flags = readCString(reader);
                *stream << BSONRegEx(pattern, flags);
            }
            break;
        }

        case CType::kDBRef: {
            size_t size = endian::bigToNative(readType<uint32_t>(reader, inverted));
            if (inverted) {
                std::unique_ptr<char[]> ns(new char[size]);
                memcpy_flipBits(ns.get(), reader->skip(size), size);
                char oidBytes[OID::kOIDSize];
                memcpy_flipBits(oidBytes, reader->skip(OID::kOIDSize), OID::kOIDSize);
                OID oid = OID::from(oidBytes);
                *stream << BSONDBRef(StringData(ns.get(), size), oid);
            } else {
                const char* ns = static_cast<const char*>(reader->skip(size));
                OID oid = OID::from(reader->skip(OID::kOIDSize));
                *stream << BSONDBRef(StringData(ns, size), oid);
            }
            break;
        }

        case CType::kObject: {
            BSONObjBuilder subObj(stream->subobjStart());
            toBson(reader, typeBits, inverted, version, &subObj);
            break;
        }

        case CType::kArray: {
            BSONObjBuilder subArr(stream->subarrayStart());
            int index = 0;
            uint8_t elemType;
            while ((elemType = readType<uint8_t>(reader, inverted)) != 0) {
                toBsonValue(elemType,
                            reader,
                            typeBits,
                            inverted,
                            version,
                            &(subArr << BSONObjBuilder::numStr(index++)));
            }
            break;
        }

        //
        // Numerics
        //

        case CType::kNumericNaN: {
            auto type = typeBits->readNumeric();
            if (type == TypeBits::kDouble) {
                *stream << std::numeric_limits<double>::quiet_NaN();
            } else {
                invariant(type == TypeBits::kDecimal && version == KeyString::Version::V1);
                *stream << Decimal128::kPositiveNaN;
            }
            break;
        }

        case CType::kNumericZero: {
            uint8_t zeroType = typeBits->readZero();
            switch (zeroType) {
                case TypeBits::kDouble:
                    *stream << 0.0;
                    break;
                case TypeBits::kInt:
                    *stream << 0;
                    break;
                case TypeBits::kLong:
                    *stream << 0ll;
                    break;
                case TypeBits::kNegativeDoubleZero:
                    *stream << -0.0;
                    break;
                default:
                    const uint32_t whichZero = typeBits->readDecimalZero(zeroType);
                    const bool isNegative = whichZero > Decimal128::kMaxBiasedExponent;
                    const uint32_t biasedExponent =
                        isNegative ? whichZero - (Decimal128::kMaxBiasedExponent + 1) : whichZero;

                    *stream << Decimal128(isNegative, biasedExponent, 0, 0);
                    break;
            }
            break;
        }

        case CType::kNumericNegativeLargeMagnitude:
            inverted = !inverted;
            isNegative = true;
        // fallthrough (format is the same as positive, but inverted)
        case CType::kNumericPositiveLargeMagnitude: {
            const uint8_t originalType = typeBits->readNumeric();
            invariant(version > KeyString::Version::V0 || originalType != TypeBits::kDecimal);
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);
            bool hasDecimalContinuation = false;
            double bin;

            // Backward compatibility
            if (version == KeyString::Version::V0) {
                memcpy(&bin, &encoded, sizeof(bin));
            } else if (!(encoded & (1ULL << 63))) {  // In range of (finite) doubles
                hasDecimalContinuation = encoded & 1;
                encoded >>= 1;          // remove decimal continuation marker
                encoded |= 1ULL << 62;  // implied leading exponent bit
                memcpy(&bin, &encoded, sizeof(bin));
                if (isNegative)
                    bin = -bin;
            } else if (encoded == ~0ULL) {  // infinity
                bin = isNegative ? -std::numeric_limits<double>::infinity()
                                 : std::numeric_limits<double>::infinity();
            } else {  // Huge decimal number, directly output
                invariant(originalType == TypeBits::kDecimal);
                uint64_t highbits = encoded & ~(1ULL << 63);
                uint64_t lowbits = endian::bigToNative(readType<uint64_t>(reader, inverted));
                Decimal128 dec(Decimal128::Value{lowbits, highbits});
                if (isNegative)
                    dec = dec.negate();
                dec = adjustDecimalExponent(typeBits, dec);
                *stream << dec;
                break;
            }

            // 'bin' contains the value of the input, rounded toward zero in case of decimal
            if (originalType == TypeBits::kDouble) {
                *stream << bin;
            } else if (originalType == TypeBits::kLong) {
                // This can only happen for a single number.
                invariant(bin == static_cast<double>(std::numeric_limits<long long>::min()));
                *stream << std::numeric_limits<long long>::min();
            } else {
                invariant(originalType == TypeBits::kDecimal && version != KeyString::Version::V0);
                const auto roundAwayFromZero = isNegative ? Decimal128::kRoundTowardNegative
                                                          : Decimal128::kRoundTowardPositive;
                Decimal128 dec(bin, Decimal128::kRoundTo34Digits, roundAwayFromZero);
                if (hasDecimalContinuation)
                    dec = readDecimalContinuation(reader, inverted, dec);
                dec = adjustDecimalExponent(typeBits, dec);
                *stream << dec;
            }
            break;
        }

        case CType::kNumericNegativeSmallMagnitude:
            inverted = !inverted;
            isNegative = true;
        // fallthrough (format is the same as positive, but inverted)

        case CType::kNumericPositiveSmallMagnitude: {
            const uint8_t originalType = typeBits->readNumeric();
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);

            if (version == KeyString::Version::V0) {
                // for these, the raw double was stored intact, including sign bit.
                invariant(originalType == TypeBits::kDouble);
                double d;
                memcpy(&d, &encoded, sizeof(d));
                *stream << d;
                break;
            }

            switch (encoded >> 62) {
                case 0x0: {
                    // Teeny tiny decimal, smaller magnitude than 2**(-1074)
                    uint64_t lowbits = readType<uint64_t>(reader, inverted);
                    lowbits = endian::bigToNative(lowbits);
                    Decimal128 dec = Decimal128(Decimal128::Value{lowbits, encoded});
                    dec = adjustDecimalExponent(typeBits, dec);
                    if (ctype == CType::kNumericNegativeSmallMagnitude)
                        dec = dec.negate();
                    *stream << dec;
                    break;
                }
                case 0x1:
                case 0x2: {
                    // Tiny double or decimal, magnitude from 2**(-1074) to 2**(-255), exclusive.
                    // The exponent is shifted by 256 in order to avoid subnormals, which would
                    // result in less than 15 significant digits. Because 2**(-255) has 179
                    // decimal digits, no doubles exactly equal decimals, so all decimals have
                    // a continuation. The bit is still needed for comparison purposes.
                    bool hasDecimalContinuation = encoded & 1;
                    encoded -= 1ULL << 62;
                    encoded >>= 1;
                    double scaledBin;
                    memcpy(&scaledBin, &encoded, sizeof(scaledBin));
                    if (originalType == TypeBits::kDouble) {
                        invariant(!hasDecimalContinuation);
                        double bin = scaledBin * kTinyDoubleExponentDownshiftFactor;
                        *stream << (isNegative ? -bin : bin);
                        break;
                    }
                    invariant(originalType == TypeBits::kDecimal && hasDecimalContinuation);

                    // If the actual double would be subnormal, scale in decimal domain.
                    Decimal128 dec;
                    if (scaledBin < DBL_MIN * kTinyDoubleExponentUpshiftFactor) {
                        // For conversion from binary->decimal scale away from zero,
                        // otherwise round toward. Needs to be done consistently in read/write.

                        Decimal128 scaledDec = Decimal128(scaledBin,
                                                          Decimal128::kRoundTo34Digits,
                                                          Decimal128::kRoundTowardPositive);
                        dec = scaledDec.multiply(kTinyDoubleExponentDownshiftFactorAsDecimal,
                                                 Decimal128::kRoundTowardZero);
                    } else {
                        double bin = scaledBin * kTinyDoubleExponentDownshiftFactor;
                        dec = Decimal128(
                            bin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardPositive);
                    }

                    dec = readDecimalContinuation(reader, inverted, dec);
                    *stream << adjustDecimalExponent(typeBits, isNegative ? dec.negate() : dec);
                    break;
                }
                case 0x3: {
                    // Small double, 2**(-255) or more in magnitude. Common case.
                    auto dcm = static_cast<KeyString::DecimalContinuationMarker>(encoded & 3);
                    encoded >>= 2;
                    double bin;
                    memcpy(&bin, &encoded, sizeof(bin));
                    if (originalType == TypeBits::kDouble) {
                        invariant(dcm == KeyString::kDCMEqualToDouble);
                        *stream << (isNegative ? -bin : bin);
                        break;
                    }

                    // Deal with decimal cases
                    invariant(originalType == TypeBits::kDecimal);
                    Decimal128 dec;
                    switch (dcm) {
                        case KeyString::kDCMEqualToDoubleRoundedUpTo15Digits:
                            dec = Decimal128(bin,
                                             Decimal128::kRoundTo15Digits,
                                             Decimal128::kRoundTowardPositive);
                            break;
                        case KeyString::kDCMEqualToDouble:
                            dec = Decimal128(bin,
                                             Decimal128::kRoundTo34Digits,
                                             Decimal128::kRoundTowardPositive);
                            break;
                        case KeyString::kDCMHasContinuationLessThanDoubleRoundedUpTo15Digits:
                        case KeyString::kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits:
                            // Deal with decimal continuation
                            dec = Decimal128(bin,
                                             Decimal128::kRoundTo34Digits,
                                             Decimal128::kRoundTowardPositive);
                            dec = readDecimalContinuation(reader, inverted, dec);
                    }
                    *stream << adjustDecimalExponent(typeBits, isNegative ? dec.negate() : dec);
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
            break;
        }

        case CType::kNumericNegative8ByteInt:
        case CType::kNumericNegative7ByteInt:
        case CType::kNumericNegative6ByteInt:
        case CType::kNumericNegative5ByteInt:
        case CType::kNumericNegative4ByteInt:
        case CType::kNumericNegative3ByteInt:
        case CType::kNumericNegative2ByteInt:
        case CType::kNumericNegative1ByteInt:
            inverted = !inverted;
            isNegative = true;
        // fallthrough (format is the same as positive, but inverted)

        case CType::kNumericPositive1ByteInt:
        case CType::kNumericPositive2ByteInt:
        case CType::kNumericPositive3ByteInt:
        case CType::kNumericPositive4ByteInt:
        case CType::kNumericPositive5ByteInt:
        case CType::kNumericPositive6ByteInt:
        case CType::kNumericPositive7ByteInt:
        case CType::kNumericPositive8ByteInt: {
            const uint8_t originalType = typeBits->readNumeric();

            uint64_t encodedIntegerPart = 0;
            {
                size_t intBytesRemaining = CType::numBytesForInt(ctype);
                while (intBytesRemaining--) {
                    encodedIntegerPart =
                        (encodedIntegerPart << 8) | readType<uint8_t>(reader, inverted);
                }
            }

            const bool haveFractionalPart = (encodedIntegerPart & 1);
            int64_t integerPart = encodedIntegerPart >> 1;

            if (!haveFractionalPart) {
                if (isNegative)
                    integerPart = -integerPart;

                switch (originalType) {
                    case TypeBits::kDouble:
                        *stream << double(integerPart);
                        break;
                    case TypeBits::kInt:
                        *stream << int(integerPart);
                        break;
                    case TypeBits::kLong:
                        *stream << static_cast<long long>(integerPart);
                        break;
                    case TypeBits::kDecimal:
                        *stream << adjustDecimalExponent(typeBits, Decimal128(integerPart));
                        break;
                    default:
                        MONGO_UNREACHABLE;
                }
                break;
            }

            // KeyString V0: anything fractional is a double
            if (version == KeyString::Version::V0) {
                invariant(originalType == TypeBits::kDouble);
                const uint64_t exponent = (64 - countLeadingZeros64(integerPart)) - 1;
                const size_t fractionalBits = (52 - exponent);
                const size_t fractionalBytes = (fractionalBits + 7) / 8;

                // build up the bits of a double here.
                uint64_t doubleBits = integerPart << fractionalBits;
                doubleBits &= ~(1ULL << 52);  // clear implicit leading 1
                doubleBits |= (exponent + 1023 /*bias*/) << 52;
                if (isNegative) {
                    doubleBits |= (1ULL << 63);  // sign bit
                }
                for (size_t i = 0; i < fractionalBytes; i++) {
                    // fold in the fractional bytes
                    const uint64_t byte = readType<uint8_t>(reader, inverted);
                    doubleBits |= (byte << ((fractionalBytes - i - 1) * 8));
                }

                double number;
                memcpy(&number, &doubleBits, sizeof(number));
                *stream << number;
                break;
            }

            // KeyString V1: all numeric values with fractions have at least 8 bytes.
            // Start with integer part, and read until we have a full 8 bytes worth of data.
            const size_t fracBytes = 8 - CType::numBytesForInt(ctype);
            uint64_t encodedFraction = integerPart;

            for (int fracBytesRemaining = fracBytes; fracBytesRemaining; fracBytesRemaining--)
                encodedFraction = (encodedFraction << 8) | readType<uint8_t>(reader, inverted);

            // Zero out the DCM and convert the whole binary fraction
            double bin = static_cast<double>(encodedFraction & ~3ULL) * kInvPow256[fracBytes];
            if (originalType == TypeBits::kDouble) {
                *stream << (isNegative ? -bin : bin);
                break;
            }

            // The two lsb's are the DCM, except for the 8-byte case, where it's already known
            KeyString::DecimalContinuationMarker dcm = fracBytes
                ? static_cast<KeyString::DecimalContinuationMarker>(encodedFraction & 3)
                : KeyString::kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;

            // Deal with decimal cases
            invariant(originalType == TypeBits::kDecimal);
            Decimal128 dec;
            switch (dcm) {
                case KeyString::kDCMEqualToDoubleRoundedUpTo15Digits:
                    dec = Decimal128(
                        bin, Decimal128::kRoundTo15Digits, Decimal128::kRoundTowardPositive);
                    break;
                case KeyString::kDCMEqualToDouble:
                    dec = Decimal128(
                        bin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardPositive);
                    break;
                default:
                    // Deal with decimal continuation
                    dec = integerPart > kMaxIntForDouble
                        ? Decimal128(integerPart)
                        : Decimal128(
                              bin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardPositive);
                    dec = readDecimalContinuation(reader, inverted, dec);
            }
            *stream << adjustDecimalExponent(typeBits, isNegative ? dec.negate() : dec);
            break;
        }
        default:
            invariant(false);
    }
}


Decimal128 adjustDecimalExponent(TypeBits::Reader* typeBits, Decimal128 num) {
    // The last 6 bits of the exponent are stored in the type bits. First figure out if the exponent
    // of 'num' is too high or too low. Even for a non-zero number with only a single significant
    // digit, there are only 34 possiblities while exponents with the given low 6 bits are spaced
    // (1 << 6) == 64 apart. This is not quite enough to figure out whether to shift the expnent up
    // or down when the difference is for example 32 in either direction. However, if the high part
    // of the coefficient is zero, the coefficient can only be scaled down by up to 1E19 (increasing
    // the exponent by 19), as 2**64 < 1E20. If scaling down to match the higher exponent isn't
    // possible, we must be able to scale up. Scaling always must be exact and not change the value.
    const uint32_t kMaxExpAdjust = 33;
    const uint32_t kMaxExpIncrementForZeroHighCoefficient = 19;
    dassert(!num.isZero());
    const uint32_t origExp = num.getBiasedExponent();
    const uint8_t storedBits = typeBits->readDecimalExponent();

    uint32_t highExp = (origExp & ~KeyString::TypeBits::kStoredDecimalExponentMask) | storedBits;

    // Start by determining an exponent that's not less than num's and matches the stored bits.
    if (highExp < origExp)
        highExp += (1U << KeyString::TypeBits::kStoredDecimalExponentBits);

    // This must be the right exponent, as no scaling is required.
    if (highExp == origExp)
        return num;

    // For increasing the exponent, quantize the existing number. This must be
    // exact, as the value stays in the same cohort.
    if (highExp <= origExp + kMaxExpAdjust &&
        (num.getCoefficientHigh() != 0 ||
         highExp <= origExp + kMaxExpIncrementForZeroHighCoefficient)) {
        // Increase exponent and decrease (right shift) coefficient.
        uint32_t flags = Decimal128::SignalingFlag::kNoFlag;
        auto quantized = num.quantize(Decimal128(0, highExp, 0, 1), &flags);
        invariant(flags == Decimal128::SignalingFlag::kNoFlag);  // must be exact
        num = quantized;
    } else {
        // Decrease exponent and increase (left shift) coefficient.
        uint32_t lowExp = highExp - (1U << KeyString::TypeBits::kStoredDecimalExponentBits);
        invariant(lowExp >= origExp - kMaxExpAdjust);
        num = num.add(Decimal128(0, lowExp, 0, 0));
    }
    dassert((num.getBiasedExponent() & KeyString::TypeBits::kStoredDecimalExponentMask) ==
            (highExp & KeyString::TypeBits::kStoredDecimalExponentMask));
    return num;
}

}  // namespace

BSONObj KeyString::toBson(const char* buffer, size_t len, Ordering ord, const TypeBits& typeBits) {
    BSONObjBuilder builder;
    BufReader reader(buffer, len);
    TypeBits::Reader typeBitsReader(typeBits);
    for (int i = 0; reader.remaining(); i++) {
        const bool invert = (ord.get(i) == -1);
        uint8_t ctype = readType<uint8_t>(&reader, invert);
        if (ctype == kLess || ctype == kGreater) {
            // This was just a discriminator which is logically part of the previous field. This
            // will only be encountered on queries, not in the keys stored in an index.
            // Note: this should probably affect the BSON key name of the last field, but it
            // must be read *after* the value so it isn't possible.
            ctype = readType<uint8_t>(&reader, invert);
        }

        if (ctype == kEnd)
            break;
        toBsonValue(ctype, &reader, &typeBitsReader, invert, typeBits.version, &(builder << ""));
    }
    return builder.obj();
}

BSONObj KeyString::toBson(StringData data, Ordering ord, const TypeBits& typeBits) {
    return toBson(data.rawData(), data.size(), ord, typeBits);
}

RecordId KeyString::decodeRecordIdAtEnd(const void* bufferRaw, size_t bufSize) {
    invariant(bufSize >= 2);  // smallest possible encoding of a RecordId.
    const unsigned char* buffer = static_cast<const unsigned char*>(bufferRaw);
    const unsigned char lastByte = *(buffer + bufSize - 1);
    const size_t ridSize = 2 + (lastByte & 0x7);  // stored in low 3 bits.
    invariant(bufSize >= ridSize);
    const unsigned char* firstBytePtr = buffer + bufSize - ridSize;
    BufReader reader(firstBytePtr, ridSize);
    return decodeRecordId(&reader);
}

RecordId KeyString::decodeRecordId(BufReader* reader) {
    const uint8_t firstByte = readType<uint8_t>(reader, false);
    const uint8_t numExtraBytes = firstByte >> 5;  // high 3 bits in firstByte
    uint64_t repr = firstByte & 0x1f;              // low 5 bits in firstByte
    for (int i = 0; i < numExtraBytes; i++) {
        repr = (repr << 8) | readType<uint8_t>(reader, false);
    }

    const uint8_t lastByte = readType<uint8_t>(reader, false);
    invariant((lastByte & 0x7) == numExtraBytes);
    repr = (repr << 5) | (lastByte >> 3);  // fold in high 5 bits of last byte
    return RecordId(repr);
}

// ----------------------------------------------------------------------
//  --------- MISC class utils --------
// ----------------------------------------------------------------------

std::string KeyString::toString() const {
    return toHex(getBuffer(), getSize());
}

int KeyString::compare(const KeyString& other) const {
    int a = getSize();
    int b = other.getSize();

    int min = std::min(a, b);

    int cmp = memcmp(getBuffer(), other.getBuffer(), min);

    if (cmp) {
        if (cmp < 0)
            return -1;
        return 1;
    }

    // keys match

    if (a == b)
        return 0;

    return a < b ? -1 : 1;
}

void KeyString::TypeBits::resetFromBuffer(BufReader* reader) {
    if (!reader->remaining()) {
        // This means AllZeros state was encoded as an empty buffer.
        reset();
        return;
    }

    const uint8_t firstByte = readType<uint8_t>(reader, false);
    if (firstByte & 0x80) {
        // firstByte is the size byte.
        _isAllZeros = false;  // it wouldn't be encoded like this if it was.

        _buf[0] = firstByte;
        const uint8_t remainingBytes = getSizeByte();
        memcpy(_buf + 1, reader->skip(remainingBytes), remainingBytes);
        return;
    }

    // In remaining cases, firstByte is the only byte.

    if (firstByte == 0) {
        // This means AllZeros state was encoded as a single 0 byte.
        reset();
        return;
    }

    _isAllZeros = false;
    setSizeByte(1);
    _buf[1] = firstByte;
}

void KeyString::TypeBits::appendBit(uint8_t oneOrZero) {
    dassert(oneOrZero == 0 || oneOrZero == 1);

    if (oneOrZero == 1)
        _isAllZeros = false;

    const uint8_t byte = (_curBit / 8) + 1;
    const uint8_t offsetInByte = _curBit % 8;
    if (offsetInByte == 0) {
        setSizeByte(byte);
        _buf[byte] = oneOrZero;  // zeros bits 1-7
    } else {
        _buf[byte] |= (oneOrZero << offsetInByte);
    }

    _curBit++;
}

void KeyString::TypeBits::appendZero(uint8_t zeroType) {
    switch (zeroType) {
        // 2-bit encodings
        case kInt:
        case kDouble:
        case kLong:
            appendBit(zeroType >> 1);
            appendBit(zeroType & 1);
            break;
        case kNegativeDoubleZero:
            if (version == Version::V0) {
                appendBit(kV0NegativeDoubleZero >> 1);
                appendBit(kV0NegativeDoubleZero & 1);
                break;
            }
            zeroType = kV1NegativeDoubleZero;
        // fallthrough for 5-bit encodings
        case kDecimalZero0xxx:
        case kDecimalZero1xxx:
        case kDecimalZero2xxx:
        case kDecimalZero3xxx:
        case kDecimalZero4xxx:
        case kDecimalZero5xxx:
            // first two bits output are ones
            dassert((zeroType >> 3) == 3);
            for (int bitPos = 4; bitPos >= 0; bitPos--)
                appendBit((zeroType >> bitPos) & 1);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void KeyString::TypeBits::appendDecimalZero(uint32_t whichZero) {
    invariant((whichZero >> 12) <= kDecimalZero5xxx - kDecimalZero0xxx);
    appendZero((whichZero >> 12) + kDecimalZero0xxx);
    for (int bitPos = 11; bitPos >= 0; bitPos--)
        appendBit((whichZero >> bitPos) & 1);
}

void KeyString::TypeBits::appendDecimalExponent(uint8_t storedExponentBits) {
    invariant(storedExponentBits < (1U << kStoredDecimalExponentBits));
    for (int bitPos = kStoredDecimalExponentBits - 1; bitPos >= 0; bitPos--)
        appendBit((storedExponentBits >> bitPos) & 1);
}

uint8_t KeyString::TypeBits::Reader::readBit() {
    if (_typeBits._isAllZeros)
        return 0;

    const uint8_t byte = (_curBit / 8) + 1;
    const uint8_t offsetInByte = _curBit % 8;
    _curBit++;

    dassert(byte <= _typeBits.getSizeByte());

    return (_typeBits._buf[byte] & (1 << offsetInByte)) ? 1 : 0;
}

uint8_t KeyString::TypeBits::Reader::readZero() {
    uint8_t res = readNumeric();

    // For keyString v1, negative and decimal zeros require at least 3 more bits.
    if (_typeBits.version != Version::V0 && res == kSpecialZeroPrefix) {
        res = (res << 1) | readBit();
        res = (res << 1) | readBit();
        res = (res << 1) | readBit();
    }
    if (res == kV1NegativeDoubleZero || res == kV0NegativeDoubleZero)
        res = kNegativeDoubleZero;
    return res;
}

uint32_t KeyString::TypeBits::Reader::readDecimalZero(uint8_t zeroType) {
    uint32_t whichZero = zeroType - TypeBits::kDecimalZero0xxx;
    for (int bitPos = 11; bitPos >= 0; bitPos--)
        whichZero = (whichZero << 1) | readBit();

    return whichZero;
}

uint8_t KeyString::TypeBits::Reader::readDecimalExponent() {
    uint8_t exponentBits = 0;
    for (int bitPos = kStoredDecimalExponentBits - 1; bitPos >= 0; bitPos--)
        exponentBits = (exponentBits << 1) | readBit();
    return exponentBits;
}
}  // namespace mongo
