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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/key_string.h"

#include <cmath>
#include <type_traits>

#include "mongo/base/data_cursor.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/hex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


#define keyStringAssert(msgid, msg, expr) \
    uassert(msgid, str::stream() << "KeyString format error: " << msg, expr)
#define keyStringAsserted(msgid, msg) \
    uasserted(msgid, str::stream() << "KeyString format error: " << msg)

namespace mongo {

using std::string;

template class StackBufBuilderBase<KeyString::TypeBits::SmallStackSize>;

namespace KeyString {


namespace {

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
MONGO_STATIC_ASSERT(kNumericPositiveLargeMagnitude < kStringLike);

const uint8_t kBoolFalse = kBool + 0;
const uint8_t kBoolTrue = kBool + 1;
MONGO_STATIC_ASSERT(kBoolTrue < kDate);

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

        case NumberDecimal:
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
    }
    MONGO_UNREACHABLE;
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
    MONGO_STATIC_ASSERT(std::is_integral<T>::value);
    T t = ConstDataView(static_cast<const char*>(reader->skip(sizeof(T)))).read<T>();
    if (inverted)
        return ~t;
    return t;
}

StringData readCString(BufReader* reader) {
    const char* start = static_cast<const char*>(reader->pos());
    const char* end = static_cast<const char*>(memchr(start, 0x0, reader->remaining()));
    keyStringAssert(50816, "Failed to find null terminator in string.", end);
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
    if (!reader->remaining() || reader->peek<unsigned char>() != 0xFF)
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
    keyStringAssert(50817, "Failed to find '0xFF' in inverted string.", end);
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
        keyStringAssert(50820, "Failed to find '0xFF' in inverted string.", end);
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

template <class BufferT>
void BuilderBase<BufferT>::resetToKey(const BSONObj& obj, Ordering ord, const RecordId& recordId) {
    resetToEmpty(ord);
    _appendAllElementsForIndexing(obj, Discriminator::kInclusive);
    appendRecordId(recordId);
}

template <class BufferT>
void BuilderBase<BufferT>::resetToKey(const BSONObj& obj,
                                      Ordering ord,
                                      Discriminator discriminator) {
    resetToEmpty(ord, discriminator);
    _appendAllElementsForIndexing(obj, discriminator);
}

template <class BufferT>
void BuilderBase<BufferT>::appendBSONElement(const BSONElement& elem, const StringTransformFn& f) {
    _verifyAppendingState();
    _appendBsonValue(elem, _shouldInvertOnAppend(), nullptr, f);
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendBool(bool val) {
    _verifyAppendingState();
    _appendBool(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendString(StringData val, const StringTransformFn& f) {
    _verifyAppendingState();
    _appendString(val, _shouldInvertOnAppend(), f);
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendSymbol(StringData val) {
    _verifyAppendingState();
    _appendSymbol(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendCode(StringData val) {
    _verifyAppendingState();
    _appendCode(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendNumberDouble(double num) {
    _verifyAppendingState();
    _appendNumberDouble(num, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendNumberLong(long long num) {
    _verifyAppendingState();
    _appendNumberLong(num, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendNumberInt(int num) {
    _verifyAppendingState();
    _appendNumberInt(num, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendNumberDecimal(Decimal128 num) {
    _verifyAppendingState();
    _appendNumberDecimal(num, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendNull() {
    _verifyAppendingState();
    _append(CType::kNullish, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendUndefined() {
    _verifyAppendingState();
    _append(CType::kUndefined, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendCodeWString(const BSONCodeWScope& val) {
    _verifyAppendingState();
    _appendCodeWString(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendBinData(const BSONBinData& data) {
    _verifyAppendingState();
    _appendBinData(data, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendRegex(const BSONRegEx& val) {
    _verifyAppendingState();
    _appendRegex(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendSetAsArray(const BSONElementSet& set, const StringTransformFn& f) {
    _verifyAppendingState();
    _appendSetAsArray(set, _shouldInvertOnAppend(), nullptr);
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendOID(OID oid) {
    _verifyAppendingState();
    _appendOID(oid, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendDate(Date_t date) {
    _verifyAppendingState();
    _appendDate(date, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendTimestamp(Timestamp val) {
    _verifyAppendingState();
    _appendTimestamp(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendBytes(const void* source, size_t bytes) {
    _verifyAppendingState();
    _appendBytes(source, bytes, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendDBRef(const BSONDBRef& val) {
    _verifyAppendingState();
    _appendDBRef(val, _shouldInvertOnAppend());
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendObject(const BSONObj& val, const StringTransformFn& f) {
    _verifyAppendingState();
    _appendObject(val, _shouldInvertOnAppend(), f);
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendArray(const BSONArray& val, const StringTransformFn& f) {
    _verifyAppendingState();
    _appendArray(val, _shouldInvertOnAppend(), f);
    _elemCount++;
}

template <class BufferT>
void BuilderBase<BufferT>::appendDiscriminator(const Discriminator discriminator) {
    // The discriminator forces this KeyString to compare Less/Greater than any KeyString with
    // the same prefix of keys. As an example, this can be used to land on the first key in the
    // index with the value "a" regardless of the RecordId. In compound indexes it can use a
    // prefix of the full key to ignore the later keys.
    switch (discriminator) {
        case Discriminator::kExclusiveBefore:
            _append(kLess, false);
            break;
        case Discriminator::kExclusiveAfter:
            _append(kGreater, false);
            break;
        case Discriminator::kInclusive:
            break;  // No discriminator byte.
    }

    // TODO (SERVER-43178): consider omitting kEnd when using a discriminator byte. It is not a
    // storage format change since keystrings with discriminators are not allowed to be stored.
    _appendEnd();
}
// ----------------------------------------------------------------------
// -----------   APPEND CODE  -------------------------------------------
// ----------------------------------------------------------------------

template <class BufferT>
void BuilderBase<BufferT>::_appendEnd() {
    _transition(BuildState::kEndAdded);
    _append(kEnd, false);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendAllElementsForIndexing(const BSONObj& obj,
                                                         Discriminator discriminator) {
    _transition(BuildState::kAppendingBSONElements);
    BSONObjIterator it(obj);
    while (auto elem = it.next()) {
        appendBSONElement(elem);
        dassert(elem.fieldNameSize() < 3);  // fieldNameSize includes the NUL
    }
    appendDiscriminator(discriminator);
}

template <class BufferT>
void BuilderBase<BufferT>::appendRecordId(const RecordId& loc) {
    _doneAppending();
    _transition(BuildState::kAppendedRecordID);
    loc.withFormat([](RecordId::Null n) { invariant(false); },
                   [&](int64_t rid) { _appendRecordIdLong(rid); },
                   [&](const char* str, int size) { _appendRecordIdStr(str, size); });
}

template <class BufferT>
void BuilderBase<BufferT>::_appendRecordIdLong(int64_t val) {
    // The RecordId encoding must be able to determine the full length starting from the last
    // byte, without knowing where the first byte is since it is stored at the end of a
    // KeyString, and we need to be able to read the RecordId without decoding the whole thing.

    // This encoding places a number (N) between 0 and 7 in both the high 3 bits of the first
    // byte and the low 3 bits of the last byte. This is the number of bytes between the first
    // and last byte (ie total bytes is N + 2). The remaining bits of the first and last bytes
    // are combined with the bits of the in-between bytes to store the 64-bit RecordId in
    // big-endian order. This does not encode negative RecordIds to give maximum space to
    // positive RecordIds which are the only ones that are allowed to be stored in an index.

    int64_t raw = val;
    if (raw < 0) {
        // Note: we encode RecordId::minLong() and RecordId() the same which is ok, as they
        // are never stored so they will never be compared to each other.
        invariant(raw == RecordId::minLong().getLong());
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

template <class BufferT>
void BuilderBase<BufferT>::_appendRecordIdStr(const char* str, int size) {
    // Append the RecordId binary string as-is, then append the encoded binary string size.
    // The binary string size is encoded in 7-bit increments over one or more size bytes.
    // The 8th bit of a size byte is a continuation bit that is set on all size bytes except
    // the leftmost (i.e. the last) one. This allows decoding the size right-to-left until there are
    // no more size bytes remaining with continuation bits. See decodeRecordIdStrAtEnd for the
    // decoding algorithm. This 7-bit size encoding ensures backward compatibility with 5.0, which
    // supports RecordId binary strings up to 127 bytes, or what fits in 7 bits.

    invariant(size > 0);
    invariant(size <= RecordId::kBigStrMaxSize);

    const bool invert = false;

    // Encode size
    uint8_t encodedSize[kRecordIdStrEncodedSizeMaxBytes] = {0};
    int highestSizeByte = 0;
    bool highestSizeByteSet = false;

    for (int sizeBytes = kRecordIdStrEncodedSizeMaxBytes - 1; sizeBytes >= 0; sizeBytes--) {
        encodedSize[sizeBytes] = (size >> (sizeBytes * 7)) & 0x7F;
        if (encodedSize[sizeBytes] && highestSizeByteSet == false) {
            highestSizeByteSet = true;
            highestSizeByte = sizeBytes;
        }
    }
    for (int i = highestSizeByte; i > 0; i--) {
        encodedSize[i] |= 0x80;
    }

    const int encodedSizeLen = highestSizeByte + 1;

    // Preallocate room for the RecordId binary string and its encoded size
    // to reduce the number of potential reallocs
    _buffer().reserveBytes(size + encodedSizeLen);
    _buffer().claimReservedBytes(size + encodedSizeLen);

    // Append RecordId and its encoded size
    _appendBytes(str, size, invert);
    _appendBytes(encodedSize, encodedSizeLen, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::appendTypeBits(const TypeBits& typeBits) {
    _transition(BuildState::kAppendedTypeBits);
    // As an optimization, encode AllZeros as a single 0 byte.
    if (typeBits.isAllZeros()) {
        _append(uint8_t(0), false);
        return;
    }

    _appendBytes(typeBits.getBuffer(), typeBits.getSize(), false);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendBool(bool val, bool invert) {
    _append(val ? CType::kBoolTrue : CType::kBoolFalse, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendDate(Date_t val, bool invert) {
    _append(CType::kDate, invert);
    // see: http://en.wikipedia.org/wiki/Offset_binary
    uint64_t encoded = static_cast<uint64_t>(val.asInt64());
    encoded ^= (1LL << 63);  // flip highest bit (equivalent to bias encoding)
    _append(endian::nativeToBig(encoded), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendTimestamp(Timestamp val, bool invert) {
    _append(CType::kTimestamp, invert);
    _append(endian::nativeToBig(val.asLL()), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendOID(OID val, bool invert) {
    _append(CType::kOID, invert);
    _appendBytes(val.view().view(), OID::kOIDSize, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendString(StringData val, bool invert, const StringTransformFn& f) {
    _typeBits.appendString();
    _append(CType::kStringLike, invert);
    if (f) {
        _appendStringLike(f(val), invert);
    } else {
        _appendStringLike(val, invert);
    }
}

template <class BufferT>
void BuilderBase<BufferT>::_appendSymbol(StringData val, bool invert) {
    _typeBits.appendSymbol();
    _append(CType::kStringLike, invert);  // Symbols and Strings compare equally
    _appendStringLike(val, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendCode(StringData val, bool invert) {
    _append(CType::kCode, invert);
    _appendStringLike(val, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendCodeWString(const BSONCodeWScope& val, bool invert) {
    _append(CType::kCodeWithScope, invert);
    _appendStringLike(val.code, invert);
    _appendBson(val.scope, invert, nullptr);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendBinData(const BSONBinData& val, bool invert) {
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

template <class BufferT>
void BuilderBase<BufferT>::_appendRegex(const BSONRegEx& val, bool invert) {
    _append(CType::kRegEx, invert);
    // note: NULL is not allowed in pattern or flags
    _appendBytes(val.pattern.rawData(), val.pattern.size(), invert);
    _append(int8_t(0), invert);
    _appendBytes(val.flags.rawData(), val.flags.size(), invert);
    _append(int8_t(0), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendDBRef(const BSONDBRef& val, bool invert) {
    _append(CType::kDBRef, invert);
    _append(endian::nativeToBig(int32_t(val.ns.size())), invert);
    _appendBytes(val.ns.rawData(), val.ns.size(), invert);
    _appendBytes(val.oid.view().view(), OID::kOIDSize, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendArray(const BSONArray& val,
                                        bool invert,
                                        const StringTransformFn& f) {
    _append(CType::kArray, invert);
    for (const auto& elem : val) {
        // No generic ctype byte needed here since no name is encoded.
        _appendBsonValue(elem, invert, nullptr, f);
    }
    _append(int8_t(0), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendSetAsArray(const BSONElementSet& val,
                                             bool invert,
                                             const StringTransformFn& f) {
    _append(CType::kArray, invert);
    for (const auto& elem : val) {
        // No generic ctype byte needed here since no name is encoded.
        _appendBsonValue(elem, invert, nullptr, f);
    }
    _append(int8_t(0), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendObject(const BSONObj& val,
                                         bool invert,
                                         const StringTransformFn& f) {
    _append(CType::kObject, invert);
    _appendBson(val, invert, f);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendNumberDouble(const double num, bool invert) {
    if (num == 0.0 && std::signbit(num))
        _typeBits.appendZero(TypeBits::kNegativeDoubleZero);
    else
        _typeBits.appendNumberDouble();

    _appendDoubleWithoutTypeBits(num, kDCMEqualToDouble, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendDoubleWithoutTypeBits(const double num,
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

template <class BufferT>
void BuilderBase<BufferT>::_appendNumberLong(const long long num, bool invert) {
    _typeBits.appendNumberLong();
    _appendInteger(num, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendNumberInt(const int num, bool invert) {
    _typeBits.appendNumberInt();
    _appendInteger(num, invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendNumberDecimal(const Decimal128 dec, bool invert) {
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
    const double bin = dec.toDouble(&signalingFlags, Decimal128::kRoundTowardZero);

    // Easy case: the decimal actually is a double. True for many integers, fractions like 1.5, etc.
    if (!Decimal128::hasFlag(signalingFlags, Decimal128::kInexact) &&
        !Decimal128::hasFlag(signalingFlags, Decimal128::kOverflow)) {
        _appendDoubleWithoutTypeBits(bin, kDCMEqualToDouble, invert);
        return;
    }

    // Values smaller than the double normalized range need special handling: a regular double
    // wouldn't give 15 digits, if any at all.
    const double absBin = std::abs(bin);
    if (absBin < std::numeric_limits<double>::min()) {
        _appendTinyDecimalWithoutTypeBits(dec, bin, invert);
        return;
    }

    // Huge finite values are encoded directly. Because the value is not exact, and truncates
    // to the maximum double, the original decimal was outside of the range of finite doubles.
    // Because all decimals larger than the max finite double round down to that value, strict
    // less-than would be incorrect.
    if (absBin >= std::numeric_limits<double>::max()) {
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

    if (absBin >= kMinLargeDouble) {
        // Large finite decimals that are not exactly equal to a double require a decimal
        // continuation, even if they only have 15 significant digits, as there only is a single bit
        // for the DCM.
        _appendLargeDouble(bin, kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits, invert);
        storedValue = Decimal128(bin, Decimal128::kRoundTo34Digits, roundAwayFromZero);

    } else if (absBin < kTiniestDoubleWith2BitDCM) {
        // Small finite decimals not exactly equal to a double similarly require a decimal
        // continuation if they're small enough to only have a single bit for the DCM.
        _appendSmallDouble(bin, kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits, invert);
        storedValue = Decimal128(bin, Decimal128::kRoundTo34Digits, roundAwayFromZero);

    } else if (absBin >= kMaxIntForDouble) {
        // For doubles in this range, 'bin' may have lost precision in the integer part, which would
        // lead to miscompares with integers. So, instead handle explicitly.
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
        _appendPreshiftedIntegerPortion(
            preshifted | static_cast<uint64_t>(hasFraction), isNegative, invert);
        if (!hasFraction)
            return;
        if (!has8bytes) {
            // A Fraction byte follows, but the leading 7 bytes already encode 53 bits of the
            // coefficient, so just store the DCM.
            uint8_t dcm = kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;
            _append(dcm, isNegative ? !invert : invert);
        }

        storedValue = Decimal128(isNegative, Decimal128::kExponentBias, 0, integerPart);

    } else if (dec.getCoefficientHigh() == 0 && dec.getCoefficientLow() < k1E15) {
        // Common case: the coefficient less than 1E15, so at most 15 digits, and the number is
        // in the range of double where we have 2 spare bits for the DCM, so the decimal can be
        // represented with at least 15 digits of precision by the double 'bin'.
        dassert(Decimal128(absBin, Decimal128::kRoundTo15Digits, Decimal128::kRoundTowardPositive)
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

template <class BufferT>
void BuilderBase<BufferT>::_appendBsonValue(const BSONElement& elem,
                                            bool invert,
                                            const StringData* name,
                                            const StringTransformFn& f) {
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
            _appendString(elem.valueStringData(), invert, f);
            break;
        case Object:
            _appendObject(elem.Obj(), invert, f);
            break;
        case Array:
            _appendArray(BSONArray(elem.Obj()), invert, f);
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
            if (f) {
                keyStringAsserted(
                    ErrorCodes::CannotBuildIndexKeys,
                    str::stream()
                        << "Cannot index type Symbol with a collation. Failed to index element: "
                        << elem << ".");
            }
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
    }
}


/// -- lowest level

template <class BufferT>
void BuilderBase<BufferT>::_appendStringLike(StringData str, bool invert) {
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

template <class BufferT>
void BuilderBase<BufferT>::_appendBson(const BSONObj& obj,
                                       bool invert,
                                       const StringTransformFn& f) {
    BSONForEach(elem, obj) {
        // Force the order to be based on (ctype, name, value).
        _append(bsonTypeToGenericKeyStringType(elem.type()), invert);
        StringData name = elem.fieldNameStringData();
        _appendBsonValue(elem, invert, &name, f);
    }
    _append(int8_t(0), invert);
}

template <class BufferT>
void BuilderBase<BufferT>::_appendSmallDouble(double value,
                                              DecimalContinuationMarker dcm,
                                              bool invert) {
    bool isNegative = value < 0;
    double magnitude = isNegative ? -value : value;
    dassert(!std::isnan(value) && value != 0 && magnitude < 1);

    _append(isNegative ? CType::kNumericNegativeSmallMagnitude
                       : CType::kNumericPositiveSmallMagnitude,
            invert);

    uint64_t encoded;

    if (version == Version::V0) {
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
        invariant(dcm != kDCMEqualToDoubleRoundedUpTo15Digits);  // only single DCM bit here
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

template <class BufferT>
void BuilderBase<BufferT>::_appendLargeDouble(double value,
                                              DecimalContinuationMarker dcm,
                                              bool invert) {
    dassert(!std::isnan(value));
    dassert(value != 0.0);
    invariant(dcm != kDCMEqualToDoubleRoundedUpTo15Digits);  // only single DCM bit here

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

template <class BufferT>
void BuilderBase<BufferT>::_appendTinyDecimalWithoutTypeBits(const Decimal128 dec,
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

    // Round down, so it is certain that in all cases a non-negative continuation can be found.
    Decimal128 storedVal(scaledBin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardZero);
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


template <class BufferT>
void BuilderBase<BufferT>::_appendHugeDecimalWithoutTypeBits(const Decimal128 dec, bool invert) {
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
template <class BufferT>
void BuilderBase<BufferT>::_appendInteger(const long long num, bool invert) {
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

template <class BufferT>
void BuilderBase<BufferT>::_appendPreshiftedIntegerPortion(uint64_t value,
                                                           bool isNegative,
                                                           bool invert) {
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


template <class BufferT>
void BuilderBase<BufferT>::_appendBytes(const void* source, size_t bytes, bool invert) {
    char* const base = _buffer().skip(bytes);

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
template <class Stream>
void toBsonValue(uint8_t ctype,
                 BufReader* reader,
                 TypeBits::Reader* typeBits,
                 bool inverted,
                 Version version,
                 Stream* stream,
                 uint32_t depth);

void toBson(BufReader* reader,
            TypeBits::Reader* typeBits,
            bool inverted,
            Version version,
            BSONObjBuilder* builder,
            uint32_t depth) {
    while (readType<uint8_t>(reader, inverted) != 0) {
        if (inverted) {
            std::string name = readInvertedCString(reader);
            BSONObjBuilderValueStream& stream = *builder << name;
            toBsonValue(readType<uint8_t>(reader, inverted),
                        reader,
                        typeBits,
                        inverted,
                        version,
                        &stream,
                        depth);
        } else {
            StringData name = readCString(reader);
            BSONObjBuilderValueStream& stream = *builder << name;
            toBsonValue(readType<uint8_t>(reader, inverted),
                        reader,
                        typeBits,
                        inverted,
                        version,
                        &stream,
                        depth);
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
    keyStringAssert(
        50850,
        "Invalid decimal continuation.",
        Decimal128::isValid(num.isNegative(), num.getBiasedExponent(), 0, continuation));
    num = num.add(Decimal128(num.isNegative(), num.getBiasedExponent(), 0, continuation), &flags);
    keyStringAssert(50815,
                    "Unexpected inexact flag set after Decimal addition.",
                    !(Decimal128::hasFlag(flags, Decimal128::kInexact)));
    return num;
}

template <class Stream>
void toBsonValue(uint8_t ctype,
                 BufReader* reader,
                 TypeBits::Reader* typeBits,
                 bool inverted,
                 Version version,
                 Stream* stream,
                 uint32_t depth) {
    keyStringAssert(ErrorCodes::Overflow,
                    "KeyString encoding exceeded maximum allowable BSON nesting depth",
                    depth <= BSONDepth::getMaxAllowableDepth());

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
                    keyStringAssert(50827,
                                    "Expected original type to be Symbol.",
                                    originalType == TypeBits::kSymbol);
                    *stream << BSONSymbol(readInvertedCStringWithNuls(reader));
                }

            } else {
                std::string scratch;
                if (originalType == TypeBits::kString) {
                    *stream << readCStringWithNuls(reader, &scratch);
                } else {
                    keyStringAssert(50828,
                                    "Expected original type to be Symbol.",
                                    originalType == TypeBits::kSymbol);
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
            // Not going to optimize CodeWScope, but limit stack space usage due to recursion.
            auto scope = std::make_unique<BSONObjBuilder>();
            // BSON validation counts a CodeWithScope as two nesting levels, so match that.
            toBson(reader, typeBits, inverted, version, scope.get(), depth + 2);
            *stream << BSONCodeWScope(code, scope->done());
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
            toBson(reader, typeBits, inverted, version, &subObj, depth + 1);
            break;
        }

        case CType::kArray: {
            BSONObjBuilder subArr(stream->subarrayStart());
            DecimalCounter<unsigned> index;
            uint8_t elemType;
            while ((elemType = readType<uint8_t>(reader, inverted)) != 0) {
                toBsonValue(elemType,
                            reader,
                            typeBits,
                            inverted,
                            version,
                            &(subArr << StringData{index}),
                            depth + 1);
                ++index;
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
                keyStringAssert(50819,
                                "Invalid type bits for numeric NaN",
                                type == TypeBits::kDecimal && version == Version::V1);
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

                    keyStringAssert(50846,
                                    "Invalid numeric zero decimal.",
                                    Decimal128::isValid(isNegative, biasedExponent, 0, 0));
                    *stream << Decimal128(isNegative, biasedExponent, 0, 0);
                    break;
            }
            break;
        }

        case CType::kNumericNegativeLargeMagnitude:
            inverted = !inverted;
            isNegative = true;
            [[fallthrough]];  // format is the same as positive, but inverted
        case CType::kNumericPositiveLargeMagnitude: {
            const uint8_t originalType = typeBits->readNumeric();
            keyStringAssert(31231,
                            "Unexpected decimal encoding for V0 KeyString.",
                            version > Version::V0 || originalType != TypeBits::kDecimal);
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);
            bool hasDecimalContinuation = false;
            double bin;

            // Backward compatibility
            if (version == Version::V0) {
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
                keyStringAssert(50818,
                                "Invalid type bits for decimal number.",
                                originalType == TypeBits::kDecimal);
                uint64_t highbits = encoded & ~(1ULL << 63);
                uint64_t lowbits = endian::bigToNative(readType<uint64_t>(reader, inverted));
                Decimal128 dec(Decimal128::Value{lowbits, highbits});
                if (isNegative)
                    dec = dec.negate();
                if (dec.isFinite())
                    dec = adjustDecimalExponent(typeBits, dec);
                *stream << dec;
                break;
            }

            // 'bin' contains the value of the input, rounded toward zero in case of decimal
            if (originalType == TypeBits::kDouble) {
                *stream << bin;
            } else if (originalType == TypeBits::kLong) {
                // This can only happen for a single number.
                keyStringAssert(50821,
                                "Unexpected value for large number.",
                                bin == static_cast<double>(std::numeric_limits<long long>::min()));
                *stream << std::numeric_limits<long long>::min();
            } else {
                keyStringAssert(50826,
                                "Unexpected type of large number.",
                                originalType == TypeBits::kDecimal && version != Version::V0);
                const auto roundAwayFromZero = isNegative ? Decimal128::kRoundTowardNegative
                                                          : Decimal128::kRoundTowardPositive;
                Decimal128 dec(bin, Decimal128::kRoundTo34Digits, roundAwayFromZero);
                if (hasDecimalContinuation)
                    dec = readDecimalContinuation(reader, inverted, dec);
                if (dec.isFinite())
                    dec = adjustDecimalExponent(typeBits, dec);
                *stream << dec;
            }
            break;
        }

        case CType::kNumericNegativeSmallMagnitude:
            inverted = !inverted;
            isNegative = true;
            [[fallthrough]];  // format is the same as positive, but inverted

        case CType::kNumericPositiveSmallMagnitude: {
            const uint8_t originalType = typeBits->readNumeric();
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);

            if (version == Version::V0) {
                // for these, the raw double was stored intact, including sign bit.
                keyStringAssert(50812,
                                "Invalid type bits for small number.",
                                originalType == TypeBits::kDouble);
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
                        keyStringAssert(
                            50822, "Unexpected decimal continuation.", !hasDecimalContinuation);
                        double bin = scaledBin * kTinyDoubleExponentDownshiftFactor;
                        *stream << (isNegative ? -bin : bin);
                        break;
                    }
                    keyStringAssert(50823,
                                    "Expected decimal continuation.",
                                    originalType == TypeBits::kDecimal && hasDecimalContinuation);

                    // If the actual double would be subnormal, scale in decimal domain.
                    Decimal128 dec;
                    if (scaledBin <
                        std::numeric_limits<double>::min() * kTinyDoubleExponentUpshiftFactor) {
                        // For subnormals, we rounded down everywhere, to ensure that the
                        // continuation will always be positive. Needs to be done consistently in
                        // encoding/decoding (see storedVal in _appendTinyDecimalWithoutTypeBits).

                        Decimal128 scaledDec = Decimal128(
                            scaledBin, Decimal128::kRoundTo34Digits, Decimal128::kRoundTowardZero);
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
                    auto dcm = static_cast<DecimalContinuationMarker>(encoded & 3);
                    encoded >>= 2;
                    double bin;
                    memcpy(&bin, &encoded, sizeof(bin));
                    if (originalType == TypeBits::kDouble) {
                        keyStringAssert(
                            50824, "Decimal contuation mismatch.", dcm == kDCMEqualToDouble);
                        *stream << (isNegative ? -bin : bin);
                        break;
                    }

                    // Deal with decimal cases
                    keyStringAssert(50825,
                                    "Unexpected type for small number, expected decimal.",
                                    originalType == TypeBits::kDecimal);
                    Decimal128 dec;
                    switch (dcm) {
                        case kDCMEqualToDoubleRoundedUpTo15Digits:
                            dec = Decimal128(bin,
                                             Decimal128::kRoundTo15Digits,
                                             Decimal128::kRoundTowardPositive);
                            break;
                        case kDCMEqualToDouble:
                            dec = Decimal128(bin,
                                             Decimal128::kRoundTo34Digits,
                                             Decimal128::kRoundTowardPositive);
                            break;
                        case kDCMHasContinuationLessThanDoubleRoundedUpTo15Digits:
                        case kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits:
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
            [[fallthrough]];  // format is the same as positive, but inverted

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
                        keyStringAsserted(50831, "Unexpected type for positive int.");
                }
                break;
            }

            // KeyString V0: anything fractional is a double
            if (version == Version::V0) {
                keyStringAssert(50832,
                                "Expected type double for fractional part.",
                                originalType == TypeBits::kDouble);
                keyStringAssert(31209,
                                "Integer part is too big to be a double.",
                                integerPart < kMaxIntForDouble);

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
            DecimalContinuationMarker dcm = fracBytes
                ? static_cast<DecimalContinuationMarker>(encodedFraction & 3)
                : kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;

            // Deal with decimal cases
            keyStringAssert(50810, "Expected type Decimal.", originalType == TypeBits::kDecimal);
            Decimal128 dec;
            switch (dcm) {
                case kDCMEqualToDoubleRoundedUpTo15Digits:
                    dec = Decimal128(
                        bin, Decimal128::kRoundTo15Digits, Decimal128::kRoundTowardPositive);
                    break;
                case kDCMEqualToDouble:
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
            keyStringAsserted(50811, str::stream() << "Unknown type: " << ctype);
    }
}

void filterKeyFromKeyString(uint8_t ctype, BufReader* reader, bool inverted, Version version);

void readBson(BufReader* reader, bool inverted, Version version) {
    while (readType<uint8_t>(reader, inverted) != 0) {
        if (inverted) {
            std::string name = readInvertedCString(reader);
            filterKeyFromKeyString(readType<uint8_t>(reader, inverted), reader, inverted, version);
        } else {
            (void)readCString(reader);
            filterKeyFromKeyString(readType<uint8_t>(reader, inverted), reader, inverted, version);
        }
    }
}

void filterKeyFromKeyString(uint8_t ctype, BufReader* reader, bool inverted, Version version) {
    switch (ctype) {
        case CType::kMinKey:
        case CType::kMaxKey:
        case CType::kNullish:
        case CType::kUndefined:
        case CType::kBoolTrue:
        case CType::kBoolFalse:
            break;

        case CType::kDate:
        case CType::kTimestamp:
            reader->skip(sizeof(std::uint64_t));
            break;

        case CType::kOID:
            reader->skip(OID::kOIDSize);
            break;

        case CType::kStringLike:
        case CType::kCode: {
            if (inverted) {
                (void)readInvertedCStringWithNuls(reader);
            } else {
                std::string scratch;
                (void)readCStringWithNuls(reader, &scratch);
            }
            break;
        }

        case CType::kCodeWithScope: {
            std::string scratch;
            if (inverted) {
                (void)readInvertedCStringWithNuls(reader);
            } else {
                (void)readCStringWithNuls(reader, &scratch);
            }
            // Not going to optimize CodeWScope.
            readBson(reader, inverted, version);
            break;
        }

        case CType::kBinData: {
            size_t size = readType<uint8_t>(reader, inverted);
            if (size == 0xff) {
                // size was stored in 4 bytes.
                size = endian::bigToNative(readType<uint32_t>(reader, inverted));
            }
            // Read the subtype of BinData
            (void)readType<uint8_t>(reader, inverted);
            // Advance by size of BinData
            reader->skip(size);
            break;
        }

        case CType::kRegEx: {
            if (inverted) {
                // Read the pattern
                (void)readInvertedCString(reader);
                // Read flags
                (void)readInvertedCString(reader);
            } else {
                // Read the pattern
                (void)readCString(reader);
                // Read flags
                (void)readCString(reader);
            }
            break;
        }

        case CType::kDBRef: {
            size_t size = endian::bigToNative(readType<uint32_t>(reader, inverted));
            reader->skip(size);
            reader->skip(OID::kOIDSize);
            break;
        }

        case CType::kObject: {
            readBson(reader, inverted, version);
            break;
        }

        case CType::kArray: {
            while (std::uint8_t elemType = readType<uint8_t>(reader, inverted)) {
                filterKeyFromKeyString(elemType, reader, inverted, version);
            }
            break;
        }

            //
            // Numerics
            //

        case CType::kNumericNaN: {
            break;
        }

        case CType::kNumericZero: {
            break;
        }

        case CType::kNumericNegativeLargeMagnitude:
            inverted = !inverted;
            [[fallthrough]];  // format is the same as positive, but inverted
        case CType::kNumericPositiveLargeMagnitude: {
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);
            bool hasDecimalContinuation = false;

            // Backward compatibility or infinity
            if (version == Version::V0 || encoded == ~0ULL) {
                break;
            } else if (!(encoded & (1ULL << 63))) {  // In range of (finite) doubles
                hasDecimalContinuation = encoded & 1;
            } else {  // Huge decimal number, get the low bits
                // It should be of type kDecimal
                (void)readType<uint64_t>(reader, inverted);
                break;
            }

            if (hasDecimalContinuation) {
                reader->skip(sizeof(std::uint64_t));
            }
            break;
        }

        case CType::kNumericNegativeSmallMagnitude:
            inverted = !inverted;
            [[fallthrough]];  // format is the same as positive, but inverted

        case CType::kNumericPositiveSmallMagnitude: {
            uint64_t encoded = readType<uint64_t>(reader, inverted);
            encoded = endian::bigToNative(encoded);

            if (version == Version::V0) {
                break;
            }

            switch (encoded >> 62) {
                case 0x0: {
                    // Teeny tiny decimal, smaller magnitude than 2**(-1074)
                    (void)readType<uint64_t>(reader, inverted);
                    break;
                }
                case 0x1:
                case 0x2: {
                    // Tiny double or decimal, magnitude from 2**(-1074) to 2**(-255), exclusive.
                    // The exponent is shifted by 256 in order to avoid subnormals, which would
                    // result in less than 15 significant digits. Because 2**(-255) has 179
                    // decimal digits, no doubles exactly equal decimals, so all decimals have
                    // a continuation.
                    bool hasDecimalContinuation = encoded & 1;
                    if (hasDecimalContinuation) {
                        reader->skip(sizeof(std::uint64_t));
                    }

                    break;
                }
                case 0x3: {
                    // Small double, 2**(-255) or more in magnitude. Common case.
                    auto dcm = static_cast<DecimalContinuationMarker>(encoded & 3);

                    // Deal with decimal cases
                    switch (dcm) {
                        case kDCMEqualToDoubleRoundedUpTo15Digits:
                        case kDCMEqualToDouble:
                            break;
                        case kDCMHasContinuationLessThanDoubleRoundedUpTo15Digits:
                        case kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits:
                            // Deal with decimal continuation
                            reader->skip(sizeof(std::uint64_t));
                    }
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
            [[fallthrough]];  // format is the same as positive, but inverted

        case CType::kNumericPositive1ByteInt:
        case CType::kNumericPositive2ByteInt:
        case CType::kNumericPositive3ByteInt:
        case CType::kNumericPositive4ByteInt:
        case CType::kNumericPositive5ByteInt:
        case CType::kNumericPositive6ByteInt:
        case CType::kNumericPositive7ByteInt:
        case CType::kNumericPositive8ByteInt: {
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
                break;
            }

            // KeyString V0: anything fractional is a double
            if (version == Version::V0) {
                const uint64_t exponent = (64 - countLeadingZeros64(integerPart)) - 1;
                const size_t fractionalBits = (52 - exponent);
                const size_t fractionalBytes = (fractionalBits + 7) / 8;

                for (size_t i = 0; i < fractionalBytes; i++) {
                    // fold in the fractional bytes
                    reader->skip(sizeof(std::uint8_t));
                }
                break;
            }

            // KeyString V1: all numeric values with fractions have at least 8 bytes.
            // Start with integer part, and read until we have a full 8 bytes worth of data.
            const size_t fracBytes = 8 - CType::numBytesForInt(ctype);
            uint64_t encodedFraction = integerPart;

            for (int fracBytesRemaining = fracBytes; fracBytesRemaining; fracBytesRemaining--)
                encodedFraction = (encodedFraction << 8) | readType<uint8_t>(reader, inverted);

            // The two lsb's are the DCM, except for the 8-byte case, where it's already known
            DecimalContinuationMarker dcm = fracBytes
                ? static_cast<DecimalContinuationMarker>(encodedFraction & 3)
                : kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits;

            // Deal with decimal cases
            switch (dcm) {
                case kDCMEqualToDoubleRoundedUpTo15Digits:
                case kDCMEqualToDouble:
                    break;
                default:
                    // Deal with decimal continuation
                    reader->skip(sizeof(std::uint64_t));
            }
            break;
        }
        default:
            MONGO_UNREACHABLE;
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
    keyStringAssert(50829, "Expected non-zero number for decimal.", !num.isZero());
    const uint32_t origExp = num.getBiasedExponent();
    const uint8_t storedBits = typeBits->readDecimalExponent();

    uint32_t highExp = (origExp & ~TypeBits::kStoredDecimalExponentMask) | storedBits;

    // Start by determining an exponent that's not less than num's and matches the stored bits.
    if (highExp < origExp)
        highExp += (1U << TypeBits::kStoredDecimalExponentBits);

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
        keyStringAssert(50845,
                        "Unexpected exponent values after adjusting Decimal.",
                        Decimal128::isValid(0, highExp, 0, 0));
        auto quantized = num.quantize(Decimal128(0, highExp, 0, 1), &flags);
        keyStringAssert(50813,
                        "Unexpected signaling flag for Decimal quantization.",
                        flags == Decimal128::SignalingFlag::kNoFlag);  // must be exact
        num = quantized;
    } else {
        // Decrease exponent and increase (left shift) coefficient.
        uint32_t lowExp = highExp - (1U << TypeBits::kStoredDecimalExponentBits);
        keyStringAssert(50814,
                        "Unexpected exponent values after adjusting Decimal.",
                        lowExp >= origExp - kMaxExpAdjust && Decimal128::isValid(0, lowExp, 0, 0));
        num = num.add(Decimal128(0, lowExp, 0, 0));
    }
    keyStringAssert(50830,
                    "Unexpected biased exponent in decimal.",
                    (num.getBiasedExponent() & TypeBits::kStoredDecimalExponentMask) ==
                        (highExp & TypeBits::kStoredDecimalExponentMask));
    return num;
}

}  // namespace

// ----------------------------------------------------------------------
//  --------- MISC class utils --------
// ----------------------------------------------------------------------

template <class BufferT>
std::string BuilderBase<BufferT>::toString() const {
    return hexblob::encode(getBuffer(), getSize());
}

std::string Value::toString() const {
    return hexblob::encode(getBuffer(), getSize());
}

TypeBits& TypeBits::operator=(const TypeBits& tb) {
    if (&tb == this) {
        return *this;
    }

    version = tb.version;
    _curBit = tb._curBit;
    _isAllZeros = tb._isAllZeros;

    _buf.reset();
    _buf.appendBuf(tb._buf.buf(), tb._buf.len());

    return *this;
}

uint32_t TypeBits::readSizeFromBuffer(BufReader* reader) {
    const uint8_t firstByte = reader->peek<uint8_t>();

    // Case 2: all bits in one byte; no size byte.
    if (firstByte > 0 && firstByte < 0x80) {
        return 1;
    }

    // Skip the indicator byte.
    reader->skip(1);

    // Case 3: <= 127 bytes; use one size byte.
    if (firstByte > 0x80) {
        return firstByte & 0x7f;
    }

    // Case 4: > 127 bytes; needs 4 size bytes.
    if (firstByte == 0x80) {
        // The next 4 bytes represent the size in little endian order.
        uint32_t s = reader->read<LittleEndian<uint32_t>>();
        keyStringAssert(50910, "Invalid overlong encoding.", s > kMaxBytesForShortEncoding);
        return s;
    }

    // Case 1: all zeros.
    dassert(firstByte == 0);
    return 0;
}


void TypeBits::setRawSize(uint32_t size) {
    // Grow the data buffer if needed.
    if (size > getDataBufferLen()) {
        _buf.grow(size - getDataBufferLen());
    }

    if (size > kMaxBytesForShortEncoding) {
        DataCursor(_buf.buf())
            .writeAndAdvance<uint8_t>(0x80)
            .writeAndAdvance<LittleEndian<uint32_t>>(size);
    } else {
        DataView(getDataBuffer() - 1).write<uint8_t>(0x80 | size);
    }
}


void TypeBits::resetFromBuffer(BufReader* reader) {
    reset();

    if (!reader->remaining())
        // This means AllZeros state was encoded as an empty buffer.
        return;

    uint32_t size = readSizeFromBuffer(reader);
    if (size > 0)
        _isAllZeros = false;
    setRawSize(size);
    memcpy(getDataBuffer(), reader->skip(size), size);
}

void TypeBits::appendBit(uint8_t oneOrZero) {
    dassert(oneOrZero == 0 || oneOrZero == 1);

    if (oneOrZero == 1)
        _isAllZeros = false;

    const uint32_t byte = _curBit / 8;
    const uint8_t offsetInByte = _curBit % 8;
    if (offsetInByte == 0) {
        setRawSize(byte + 1);
        getDataBuffer()[byte] = oneOrZero;  // zeros bits 1-7
    } else {
        getDataBuffer()[byte] |= (oneOrZero << offsetInByte);
    }

    _curBit++;
}

void TypeBits::appendZero(uint8_t zeroType) {
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
            [[fallthrough]];  // fallthrough for 5-bit encodings
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

void TypeBits::appendDecimalZero(uint32_t whichZero) {
    invariant((whichZero >> 12) <= kDecimalZero5xxx - kDecimalZero0xxx);
    appendZero((whichZero >> 12) + kDecimalZero0xxx);
    for (int bitPos = 11; bitPos >= 0; bitPos--)
        appendBit((whichZero >> bitPos) & 1);
}

void TypeBits::appendDecimalExponent(uint8_t storedExponentBits) {
    invariant(storedExponentBits < (1U << kStoredDecimalExponentBits));
    for (int bitPos = kStoredDecimalExponentBits - 1; bitPos >= 0; bitPos--)
        appendBit((storedExponentBits >> bitPos) & 1);
}

uint8_t TypeBits::Reader::readBit() {
    if (_typeBits._isAllZeros)
        return 0;

    const uint32_t byte = _curBit / 8;
    const uint8_t offsetInByte = _curBit % 8;
    _curBit++;

    keyStringAssert(50615, "Invalid size byte(s).", byte < _typeBits.getDataBufferLen());

    return (_typeBits.getDataBuffer()[byte] & (1 << offsetInByte)) ? 1 : 0;
}

uint8_t TypeBits::Reader::readZero() {
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

uint32_t TypeBits::Reader::readDecimalZero(uint8_t zeroType) {
    uint32_t whichZero = zeroType - kDecimalZero0xxx;
    for (int bitPos = 11; bitPos >= 0; bitPos--)
        whichZero = (whichZero << 1) | readBit();

    return whichZero;
}

uint8_t TypeBits::Reader::readDecimalExponent() {
    uint8_t exponentBits = 0;
    for (int bitPos = kStoredDecimalExponentBits - 1; bitPos >= 0; bitPos--)
        exponentBits = (exponentBits << 1) | readBit();
    return exponentBits;
}

size_t getKeySize(const char* buffer, size_t len, Ordering ord, const TypeBits& typeBits) {
    invariant(len > 0);
    BufReader reader(buffer, len);
    unsigned remainingBytes;
    for (int i = 0; (remainingBytes = reader.remaining()); i++) {
        const bool invert = (ord.get(i) == -1);
        uint8_t ctype = readType<uint8_t>(&reader, invert);
        // We have already read the Key.
        if (ctype == kEnd)
            break;

        // Read the Key that comes after the first byte in KeyString.
        filterKeyFromKeyString(ctype, &reader, invert, typeBits.version);
    }

    invariant(len > remainingBytes);
    // Key size = buffer len - number of bytes comprising the RecordId
    return (len - (remainingBytes - 1));
}

// This discriminator byte only exists in KeyStrings for queries, not in KeyStrings stored in an
// index.
Discriminator decodeDiscriminator(const char* buffer,
                                  size_t len,
                                  Ordering ord,
                                  const TypeBits& typeBits) {
    BufReader reader(buffer, len);
    TypeBits::Reader typeBitsReader(typeBits);
    for (int i = 0; reader.remaining(); i++) {
        const bool invert = (ord.get(i) == -1);
        uint8_t ctype = readType<uint8_t>(&reader, invert);
        if (ctype == kLess || ctype == kGreater) {
            // Invert it back if discriminator byte got mistakenly inverted.
            if (invert)
                ctype = ~ctype;
            return ctype == kLess ? Discriminator::kExclusiveBefore
                                  : Discriminator::kExclusiveAfter;
        }

        if (ctype == kEnd)
            break;
        filterKeyFromKeyString(ctype, &reader, invert, typeBits.version);
    }
    return Discriminator::kInclusive;
}

void toBsonSafe(const char* buffer,
                size_t len,
                Ordering ord,
                const TypeBits& typeBits,
                BSONObjBuilder& builder) {
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
        toBsonValue(ctype, &reader, &typeBitsReader, invert, typeBits.version, &(builder << ""), 1);
    }
}

BSONObj toBsonSafe(const char* buffer, size_t len, Ordering ord, const TypeBits& typeBits) {
    BSONObjBuilder builder;
    toBsonSafe(buffer, len, ord, typeBits, builder);
    return builder.obj();
}

BSONObj toBson(const char* buffer, size_t len, Ordering ord, const TypeBits& typeBits) noexcept {
    return toBsonSafe(buffer, len, ord, typeBits);
}

BSONObj toBson(StringData data, Ordering ord, const TypeBits& typeBits) {
    return toBson(data.rawData(), data.size(), ord, typeBits);
}

RecordId decodeRecordIdLongAtEnd(const void* bufferRaw, size_t bufSize) {
    const unsigned char* buffer = static_cast<const unsigned char*>(bufferRaw);
    invariant(bufSize >= 2);  // smallest possible encoding of a RecordId.
    const unsigned char lastByte = *(buffer + bufSize - 1);
    const size_t ridSize = 2 + (lastByte & 0x7);  // stored in low 3 bits.
    invariant(bufSize >= ridSize);
    const unsigned char* firstBytePtr = buffer + bufSize - ridSize;
    BufReader reader(firstBytePtr, ridSize);
    return decodeRecordIdLong(&reader);
}

size_t sizeWithoutRecordIdLongAtEnd(const void* bufferRaw, size_t bufSize) {
    invariant(bufSize >= 2);  // smallest possible encoding of a RecordId.
    const unsigned char* buffer = static_cast<const unsigned char*>(bufferRaw);
    const unsigned char lastByte = *(buffer + bufSize - 1);
    const size_t ridSize = 2 + (lastByte & 0x7);  // stored in low 3 bits.
    invariant(bufSize >= ridSize);
    return bufSize - ridSize;
}

size_t sizeWithoutRecordIdStrAtEnd(const void* bufferRaw, size_t bufSize) {
    // See decodeRecordIdStrAtEnd for the size decoding algorithm
    invariant(bufSize > 0);
    const uint8_t* buffer = static_cast<const uint8_t*>(bufferRaw);

    // Decode RecordId binary string size
    size_t ridSize = 0;
    uint8_t sizes[kRecordIdStrEncodedSizeMaxBytes] = {0};

    // Continuation bytes
    size_t sizeByteId = 0;
    for (; buffer[bufSize - 1 - sizeByteId] & 0x80; sizeByteId++) {
        invariant(bufSize >= sizeByteId + 1 /* non-cont bytes */);
        invariant(sizeByteId < kRecordIdStrEncodedSizeMaxBytes);
        sizes[sizeByteId] = buffer[bufSize - 1 - sizeByteId] & 0x7F;
    }
    // Last (non-continuation) byte
    invariant(sizeByteId < kRecordIdStrEncodedSizeMaxBytes);
    sizes[sizeByteId] = buffer[bufSize - 1 - sizeByteId];

    const size_t numSegments = sizeByteId + 1;

    for (; sizeByteId > 0; sizeByteId--) {
        ridSize += static_cast<size_t>(sizes[sizeByteId]) << ((numSegments - sizeByteId - 1) * 7);
    }
    ridSize += static_cast<size_t>(sizes[sizeByteId]) << ((numSegments - sizeByteId - 1) * 7);

    invariant(bufSize >= ridSize + numSegments);
    return bufSize - ridSize - numSegments;
}

RecordId decodeRecordIdLong(BufReader* reader) {
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

RecordId decodeRecordIdStrAtEnd(const void* bufferRaw, size_t bufSize) {
    // See _appendRecordIdStr for the encoding scheme.
    // The RecordId binary string size is decoded right-to-left, up to the size byte
    // without continuation bit.

    invariant(bufSize > 0);
    const uint8_t* buffer = static_cast<const uint8_t*>(bufferRaw);

    // Decode RecordId binary string size
    size_t ridSize = 0;
    uint8_t sizes[kRecordIdStrEncodedSizeMaxBytes] = {0};

    // Continuation bytes
    size_t sizeByteId = 0;
    for (; buffer[bufSize - 1 - sizeByteId] & 0x80; sizeByteId++) {
        invariant(bufSize >= sizeByteId + 1 /* non-cont byte */);
        invariant(sizeByteId < kRecordIdStrEncodedSizeMaxBytes);
        sizes[sizeByteId] = buffer[bufSize - 1 - sizeByteId] & 0x7F;
    }
    // Last (non-continuation) byte
    invariant(sizeByteId < kRecordIdStrEncodedSizeMaxBytes);
    sizes[sizeByteId] = buffer[bufSize - 1 - sizeByteId];

    const size_t numSegments = sizeByteId + 1;

    for (; sizeByteId > 0; sizeByteId--) {
        ridSize += static_cast<size_t>(sizes[sizeByteId]) << ((numSegments - sizeByteId - 1) * 7);
    }
    ridSize += static_cast<size_t>(sizes[sizeByteId]) << ((numSegments - sizeByteId - 1) * 7);

    invariant(bufSize >= ridSize + numSegments);

    return RecordId(reinterpret_cast<const char*>(buffer) + (bufSize - ridSize - numSegments),
                    ridSize);
}

int compare(const char* leftBuf, const char* rightBuf, size_t leftSize, size_t rightSize) {
    // memcmp has undefined behavior if either leftBuf or rightBuf is a null pointer.
    if (MONGO_unlikely(leftSize == 0))
        return rightSize == 0 ? 0 : -1;
    else if (MONGO_unlikely(rightSize == 0))
        return 1;

    int min = std::min(leftSize, rightSize);

    int cmp = memcmp(leftBuf, rightBuf, min);

    if (cmp) {
        if (cmp < 0)
            return -1;
        return 1;
    }

    // keys match

    if (leftSize == rightSize)
        return 0;

    return leftSize < rightSize ? -1 : 1;
}

int Value::compareWithTypeBits(const Value& other) const {
    return KeyString::compare(getBuffer(), other.getBuffer(), _buffer.size(), other._buffer.size());
}

bool readSBEValue(BufReader* reader,
                  TypeBits::Reader* typeBits,
                  bool inverted,
                  Version version,
                  sbe::value::ValueBuilder* valueBuilder) {
    uint8_t ctype;
    if (!reader->remaining() || (ctype = readType<uint8_t>(reader, inverted)) == kEnd) {
        return false;
    }

    // This function is only intended to read stored index entries. The 'kLess' and 'kGreater'
    // "discriminator" types are used for querying and are never stored in an index.
    invariant(ctype > kLess && ctype < kGreater);

    const uint32_t depth = 1;  // This function only gets called for a top-level KeyString::Value.
    toBsonValue(ctype, reader, typeBits, inverted, version, valueBuilder, depth);
    return true;
}

void appendSingleFieldToBSONAs(
    const char* buf, int len, StringData fieldName, BSONObjBuilder* builder, Version version) {
    const bool inverted = false;

    BufReader reader(buf, len);
    invariant(reader.remaining());
    uint8_t ctype = readType<uint8_t>(&reader, inverted);
    invariant(ctype != kEnd && ctype > kLess && ctype < kGreater);

    const uint32_t depth = 1;  // This function only gets called for a top-level KeyString::Value.
    // Callers discard their TypeBits.
    TypeBits typeBits(version);
    TypeBits::Reader typeBitsReader(typeBits);

    BSONObjBuilderValueStream& stream = *builder << fieldName;
    toBsonValue(ctype, &reader, &typeBitsReader, inverted, version, &stream, depth);
}

void appendToBSONArray(const char* buf, int len, BSONArrayBuilder* builder, Version version) {
    const bool inverted = false;

    BufReader reader(buf, len);
    invariant(reader.remaining());
    uint8_t ctype = readType<uint8_t>(&reader, inverted);
    invariant(ctype != kEnd && ctype > kLess && ctype < kGreater);

    // This function only gets called for a top-level KeyString::Value.
    const uint32_t depth = 1;
    // All users of this currently discard type bits.
    TypeBits typeBits(version);
    TypeBits::Reader typeBitsReader(typeBits);

    toBsonValue(ctype, &reader, &typeBitsReader, inverted, version, builder, depth);
}

void Value::serializeWithoutRecordIdLong(BufBuilder& buf) const {
    dassert(decodeRecordIdLongAtEnd(_buffer.get(), _ksSize).isValid());

    const int32_t sizeWithoutRecordId = sizeWithoutRecordIdLongAtEnd(_buffer.get(), _ksSize);
    buf.appendNum(sizeWithoutRecordId);                 // Serialize size of KeyString
    buf.appendBuf(_buffer.get(), sizeWithoutRecordId);  // Serialize KeyString
    buf.appendBuf(_buffer.get() + _ksSize, _buffer.size() - _ksSize);  // Serialize TypeBits
}

void Value::serializeWithoutRecordIdStr(BufBuilder& buf) const {
    dassert(decodeRecordIdStrAtEnd(_buffer.get(), _ksSize).isValid());

    const int32_t sizeWithoutRecordId = sizeWithoutRecordIdStrAtEnd(_buffer.get(), _ksSize);
    buf.appendNum(sizeWithoutRecordId);                 // Serialize size of KeyString
    buf.appendBuf(_buffer.get(), sizeWithoutRecordId);  // Serialize KeyString
    buf.appendBuf(_buffer.get() + _ksSize, _buffer.size() - _ksSize);  // Serialize TypeBits
}

size_t Value::getApproximateSize() const {
    auto size = sizeof(Value);
    size += !_buffer.isShared() ? SharedBuffer::kHolderSize + _buffer.size() : 0;
    return size;
}

template class BuilderBase<Builder>;
template class BuilderBase<HeapBuilder>;
template class BuilderBase<PooledBuilder>;

void logKeyString(const RecordId& recordId,
                  const Value& keyStringValue,
                  const BSONObj& keyPatternBson,
                  const BSONObj& keyStringBson,
                  std::string callerLogPrefix) {
    // We need to rehydrate the keyString to something readable.
    auto keyPatternIter = keyPatternBson.begin();
    auto keyStringIter = keyStringBson.begin();
    BSONObjBuilder b;
    while (keyPatternIter != keyPatternBson.end() && keyStringIter != keyStringBson.end()) {
        b.appendAs(*keyStringIter, keyPatternIter->fieldName());
        ++keyPatternIter;
        ++keyStringIter;
    }
    // Wildcard index documents can have more values in the keystring.
    while (keyStringIter != keyStringBson.end()) {
        b.append(*keyStringIter);
        ++keyStringIter;
    }
    BSONObj rehydratedKey = b.done();

    LOGV2(51811,
          "{caller} {record_id}, key: {rehydrated_key}, keystring: "
          "{key_string}",
          "caller"_attr = callerLogPrefix,
          "record_id"_attr = recordId,
          "rehydrated_key"_attr = rehydratedKey,
          "key_string"_attr = keyStringValue);
}

}  // namespace KeyString

}  // namespace mongo
