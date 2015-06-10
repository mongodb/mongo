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
            const uint8_t kNumericNegativeLargeDouble = kNumeric + 1; // <= -2**63 including -Inf
            const uint8_t kNumericNegative8ByteInt = kNumeric + 2;
            const uint8_t kNumericNegative7ByteInt = kNumeric + 3;
            const uint8_t kNumericNegative6ByteInt = kNumeric + 4;
            const uint8_t kNumericNegative5ByteInt = kNumeric + 5;
            const uint8_t kNumericNegative4ByteInt = kNumeric + 6;
            const uint8_t kNumericNegative3ByteInt = kNumeric + 7;
            const uint8_t kNumericNegative2ByteInt = kNumeric + 8;
            const uint8_t kNumericNegative1ByteInt = kNumeric + 9;
            const uint8_t kNumericNegativeSmallDouble = kNumeric + 10; // between 0 and -1 exclusive
            const uint8_t kNumericZero = kNumeric + 11;
            const uint8_t kNumericPositiveSmallDouble = kNumeric + 12; // between 0 and 1 exclusive
            const uint8_t kNumericPositive1ByteInt = kNumeric + 13;
            const uint8_t kNumericPositive2ByteInt = kNumeric + 14;
            const uint8_t kNumericPositive3ByteInt = kNumeric + 15;
            const uint8_t kNumericPositive4ByteInt = kNumeric + 16;
            const uint8_t kNumericPositive5ByteInt = kNumeric + 17;
            const uint8_t kNumericPositive6ByteInt = kNumeric + 18;
            const uint8_t kNumericPositive7ByteInt = kNumeric + 19;
            const uint8_t kNumericPositive8ByteInt = kNumeric + 20;
            const uint8_t kNumericPositiveLargeDouble = kNumeric + 21; // >= 2**63 including +Inf
            BOOST_STATIC_ASSERT(kNumericPositiveLargeDouble < kStringLike);

            const uint8_t kBoolFalse = kBool + 0;
            const uint8_t kBoolTrue = kBool + 1;
            BOOST_STATIC_ASSERT(kBoolTrue < kDate);

            size_t numBytesForInt(uint8_t ctype) {
                if (ctype >= kNumericPositive1ByteInt) {
                    dassert(ctype <= kNumericPositive8ByteInt);
                    return ctype - kNumericPositive1ByteInt + 1;
                }

                dassert(ctype <= kNumericNegative1ByteInt);
                dassert(ctype >= kNumericNegative8ByteInt);
                return kNumericNegative1ByteInt - ctype + 1;
            }
        } // namespace CType

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

            case Object: return CType::kObject;
            case Array: return CType::kArray;
            case BinData: return CType::kBinData;
            case jstOID: return CType::kOID;
            case Bool: return CType::kBool;
            case Date: return CType::kDate;
            case bsonTimestamp: return CType::kTimestamp;
            case RegEx: return CType::kRegEx;
            case DBRef: return CType::kDBRef;

            case Code: return CType::kCode;
            case CodeWScope: return CType::kCodeWithScope;

            case MaxKey: return CType::kMaxKey;
            default:
                invariant(false);
            }
        }

        // First double that isn't an int64.
        const double kMinLargeDouble = 9223372036854775808.0; // 1ULL<<63

        const uint8_t kEnd = 0x4;

        // These overlay with CType or kEnd bytes and therefor must be less/greater than all of
        // them (and their inverses). They also can't equal 0 or 255 since that would collide with
        // the encoding of NUL bytes in strings as "\x00\xff".
        const uint8_t kLess = 1;
        const uint8_t kGreater = 254;
    } // namespace

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

        template <typename T> T readType(BufReader* reader, bool inverted) {
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
                return initial; // Don't alloc or copy for simple case with no NUL bytes.

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
            do {
                if (!out.empty()) {
                    // If this isn't our first pass through the loop it means we hit an NUL byte
                    // encoded as "\xFF\00" in our inverted string.
                    reader->skip(1);
                    out += '\xFF'; // will be flipped to '\0' with rest of out before returning.
                }

                const char* start = static_cast<const char*>(reader->pos());
                const char* end = static_cast<const char*>(
                                    memchr(start, 0xFF, reader->remaining()));
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
    } // namespace

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

    void KeyString::_appendAllElementsForIndexing(const BSONObj& obj, Ordering ord,
                                                  Discriminator discriminator) {
        int elemCount = 0;
        BSONObjIterator it(obj);
        while (auto elem = it.next()) {
            const int elemIdx = elemCount++;
            const bool invert = (ord.get(elemIdx) == -1);

            _appendBsonValue(elem, invert, NULL);

            dassert(elem.fieldNameSize() < 3); // fieldNameSize includes the NUL

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
        case kExclusiveBefore: _append(kLess, false); break;
        case kExclusiveAfter: _append(kGreater, false); break;
        case kInclusive: break; // No discriminator byte.
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
        const int extraBytesNeeded = bitsNeeded <= 10
                                     ? 0
                                     : ((bitsNeeded - 10) + 7) / 8; // ceil((bitsNeeded - 10) / 8)

        // extraBytesNeeded must fit in 3 bits.
        dassert(extraBytesNeeded >= 0 && extraBytesNeeded < 8);

        // firstByte combines highest 5 bits of value with extraBytesNeeded.
        const uint8_t firstByte = uint8_t((extraBytesNeeded << 5)
                                          | (value >> (5 + (extraBytesNeeded * 8))));
        // lastByte combines lowest 5 bits of value with extraBytesNeeded.
        const uint8_t lastByte = uint8_t((value << 3) | extraBytesNeeded);

        // RecordIds are never appended inverted.
        _append(firstByte, false);
        if (extraBytesNeeded) {
            const uint64_t extraBytes = endian::nativeToBig(value >> 5);
            // Only using the low-order extraBytesNeeded bytes of extraBytes.
            _appendBytes(reinterpret_cast<const char*>(&extraBytes) + sizeof(extraBytes)
                                                                    - extraBytesNeeded,
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
        encoded ^= (1LL << 63); // flip highest bit (equivalent to bias encoding)
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
        _append(CType::kStringLike, invert); // Symbols and Strings compare equally
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
        }
        else {
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
        if (num == 0.0 && std::signbit(num)) {
            _typeBits.appendNegativeZero();
        }
        else {
            _typeBits.appendNumberDouble();
        }

        // no special cases needed for Inf,
        // see http://en.wikipedia.org/wiki/IEEE_754-1985#Positive_and_negative_infinity
        if (std::isnan(num)) {
            _append(CType::kNumericNaN, invert);
            return;
        }

        if (num == 0.0) {
            // We are collapsing -0.0 and 0.0 to the same value here.
            // This is correct as IEEE-754 specifies that they compare as equal,
            // however this prevents roundtripping -0.0.
            // So if you put a -0.0 in, you'll get 0.0 out.
            // We believe this to be ok.
            _append(CType::kNumericZero, invert);
            return;
        }

        const bool isNegative = num < 0.0;
        const double magnitude = isNegative ? -num : num;

        if (magnitude < 1.0) {
            // This includes subnormal numbers.
            _appendSmallDouble(num, invert);
            return;
        }

        if (magnitude < kMinLargeDouble) {
            uint64_t integerPart = uint64_t(magnitude);
            if (double(integerPart) == magnitude) {
                // No fractional part
                _appendPreshiftedIntegerPortion(integerPart << 1, isNegative, invert);
                return;
            }

            // There is a fractional part.
            _appendPreshiftedIntegerPortion((integerPart << 1) | 1, isNegative, invert);

            // Append the bytes of the mantissa that include fractional bits.
            const size_t fractionalBits = (53 - (64 - countLeadingZeros64(integerPart)));
            const size_t fractionalBytes = (fractionalBits + 7) / 8;
            dassert(fractionalBytes > 0);
            uint64_t mantissa;
            memcpy(&mantissa, &num, sizeof(mantissa));
            mantissa &= ~(uint64_t(-1) << fractionalBits); // set non-fractional bits to 0;
            mantissa = endian::nativeToBig(mantissa);

            const void* firstUsedByte =
                reinterpret_cast<const char*>((&mantissa) + 1) - fractionalBytes;
            _appendBytes(firstUsedByte, fractionalBytes, isNegative ? !invert : invert);
            return;
        }

        _appendLargeDouble(num, invert);
    }

    void KeyString::_appendNumberLong(const long long num, bool invert) {
        _typeBits.appendNumberLong();
        _appendInteger(num, invert);
    }

    void KeyString::_appendNumberInt(const int num, bool invert) {
        _typeBits.appendNumberInt();
        _appendInteger(num, invert);
    }

    void KeyString::_appendBsonValue(const BSONElement& elem,
                                     bool invert,
                                     const StringData* name) {

        if (name) {
            _appendBytes(name->rawData(), name->size() + 1, invert); // + 1 for NUL
        }

        switch (elem.type()) {
        case MinKey:
        case MaxKey:
        case EOO:
        case Undefined:
        case jstNULL:
            _append(bsonTypeToGenericKeyStringType(elem.type()), invert);
            break;

        case NumberDouble: _appendNumberDouble(elem._numberDouble(), invert); break;
        case String: _appendString(elem.valueStringData(), invert); break;
        case Object: _appendObject(elem.Obj(), invert); break;
        case Array: _appendArray(BSONArray(elem.Obj()), invert); break;
        case BinData: {
            int len;
            const char* data = elem.binData(len);
            _appendBinData(BSONBinData(data, len, elem.binDataType()), invert);
            break;
        }

        case jstOID: _appendOID(elem.__oid(), invert); break;
        case Bool: _appendBool(elem.boolean(), invert); break;
        case Date: _appendDate(elem.date(), invert); break;

        case RegEx: _appendRegex(BSONRegEx(elem.regex(), elem.regexFlags()), invert); break;
        case DBRef: _appendDBRef(BSONDBRef(elem.dbrefNS(), elem.dbrefOID()), invert); break;
        case Symbol: _appendSymbol(elem.valueStringData(), invert); break;
        case Code: _appendCode(elem.valueStringData(), invert); break;
        case CodeWScope: {
            _appendCodeWString(BSONCodeWScope(StringData(elem.codeWScopeCode(),
                                                         elem.codeWScopeCodeLen()-1),
                                              BSONObj(elem.codeWScopeScopeData())),
                               invert);
            break;
        }
        case NumberInt: _appendNumberInt(elem._numberInt(), invert); break;
        case bsonTimestamp: _appendTimestamp(elem.timestamp(), invert); break;
        case NumberLong: _appendNumberLong(elem._numberLong(), invert); break;

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
            str = str.substr(firstNul + 1); // skip over the NUL byte
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

    void KeyString::_appendSmallDouble(double value, bool invert) {
        dassert(!std::isnan(value));
        dassert(value != 0.0);

        uint64_t data;
        memcpy(&data, &value, sizeof(data));

        if (value > 0) {
            _append(CType::kNumericPositiveSmallDouble, invert);
            _append(endian::nativeToBig(data), invert);
        }
        else {
            _append(CType::kNumericNegativeSmallDouble, invert);
            _append(endian::nativeToBig(data), !invert);
        }
    }

    void KeyString::_appendLargeDouble(double value, bool invert) {
        dassert(!std::isnan(value));
        dassert(value != 0.0);

        uint64_t data;
        memcpy(&data, &value, sizeof(data));

        if (value > 0) {
            _append(CType::kNumericPositiveLargeDouble, invert);
            _append(endian::nativeToBig(data), invert);
        }
        else {
            _append(CType::kNumericNegativeLargeDouble, invert);
            _append(endian::nativeToBig(data), !invert);
        }
    }

    // Handles NumberLong and NumberInt which are encoded identically except for the TypeBits.
    void KeyString::_appendInteger(const long long num, bool invert) {
        if (num == std::numeric_limits<long long>::min()) {
            // -2**63 is exactly representable as a double and not as a positive int64.
            // Therefore we encode it as a double.
            dassert(-double(num) == kMinLargeDouble);
            _appendLargeDouble(double(num), invert);
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
        dassert(value != 0ull);
        dassert(value != 1ull);

        const size_t bytesNeeded = (64 - countLeadingZeros64(value) + 7) / 8;

        // Append the low bytes of value in big endian order.
        value = endian::nativeToBig(value);
        const void* firstUsedByte = reinterpret_cast<const char*>((&value) + 1) - bytesNeeded;

        if (isNegative) {
            _append(uint8_t(CType::kNumericNegative1ByteInt - (bytesNeeded - 1)), invert);
            _appendBytes(firstUsedByte, bytesNeeded, !invert);
        }
        else {
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
                         BSONObjBuilderValueStream* stream);

        void toBson(BufReader* reader, TypeBits::Reader* typeBits,
                    bool inverted, BSONObjBuilder* builder) {
            while (readType<uint8_t>(reader, inverted) != 0) {
                if (inverted) {
                    std::string name = readInvertedCString(reader);
                    BSONObjBuilderValueStream& stream = *builder << name;
                    toBsonValue(readType<uint8_t>(reader, inverted), reader, typeBits, inverted,
                                                  &stream);
                }
                else {
                    StringData name = readCString(reader);
                    BSONObjBuilderValueStream& stream = *builder << name;
                    toBsonValue(readType<uint8_t>(reader, inverted), reader, typeBits, inverted,
                                &stream);
                }
            }
        }

        void toBsonValue(uint8_t ctype,
                         BufReader* reader,
                         TypeBits::Reader* typeBits,
                         bool inverted,
                         BSONObjBuilderValueStream* stream) {

            // This is only used by the kNumeric.*ByteInt types, but needs to be declared up here
            // since it is used across a fallthrough.
            bool isNegative = false;

            switch (ctype) {
            case CType::kMinKey: *stream << MINKEY; break;
            case CType::kMaxKey: *stream << MAXKEY; break;
            case CType::kNullish: *stream << BSONNULL; break;
            case CType::kUndefined: *stream << BSONUndefined; break;

            case CType::kBoolTrue: *stream << true; break;
            case CType::kBoolFalse: *stream << false; break;

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
                }
                else {
                    *stream << OID::from(reader->skip(OID::kOIDSize));
                }
                break;

            case CType::kStringLike: {
                const uint8_t originalType = typeBits->readStringLike();
                if (inverted) {
                    if (originalType == TypeBits::kString) {
                        *stream << readInvertedCStringWithNuls(reader);
                    }
                    else {
                        dassert(originalType == TypeBits::kSymbol);
                        *stream << BSONSymbol(readInvertedCStringWithNuls(reader));
                    }
                    
                }
                else  {
                    std::string scratch;
                    if (originalType == TypeBits::kString) {
                        *stream << readCStringWithNuls(reader, &scratch);
                    }
                    else {
                        dassert(originalType == TypeBits::kSymbol);
                        *stream << BSONSymbol(readCStringWithNuls(reader, &scratch));
                    }
                }
                break;
            }

            case CType::kCode: {
                if (inverted) {
                    *stream << BSONCode(readInvertedCStringWithNuls(reader));
                }
                else {
                    std::string scratch;
                    *stream << BSONCode(readCStringWithNuls(reader, &scratch));
                }
                break;
            }

            case CType::kCodeWithScope: {
                std::string scratch;
                StringData code; // will point to either scratch or the raw encoded bytes.
                if (inverted) {
                    scratch = readInvertedCStringWithNuls(reader);
                    code = scratch;
                }
                else {
                    code = readCStringWithNuls(reader, &scratch);
                }
                // Not going to optimize CodeWScope.
                BSONObjBuilder scope;
                toBson(reader, typeBits, inverted, &scope);
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
                }
                else {
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
                }
                else {
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
                }
                else {
                    const char* ns = static_cast<const char*>(reader->skip(size));
                    OID oid = OID::from(reader->skip(OID::kOIDSize));
                    *stream << BSONDBRef(StringData(ns, size), oid);
                }
                break;
            }

            case CType::kObject: {
                BSONObjBuilder subObj(stream->subobjStart());
                toBson(reader, typeBits, inverted, &subObj);
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
                                &(subArr << BSONObjBuilder::numStr(index++)));
                }
                break;
            }

            //
            // Numerics
            //

            case CType::kNumericNaN:
                invariant(typeBits->readNumeric() == TypeBits::kDouble);
                *stream << std::numeric_limits<double>::quiet_NaN();
                break;

            case CType::kNumericZero:
                switch(typeBits->readNumeric()) {
                case TypeBits::kDouble: *stream << 0.0; break;
                case TypeBits::kInt: *stream << 0; break;
                case TypeBits::kLong: *stream << 0ll; break;
                case TypeBits::kNegativeZero: *stream << -0.0; break;
                }
                break;

            case CType::kNumericNegativeLargeDouble:
            case CType::kNumericNegativeSmallDouble:
                inverted = !inverted;
                // fallthrough (format is the same as positive, but inverted)

            case CType::kNumericPositiveLargeDouble:
            case CType::kNumericPositiveSmallDouble: {
                // for these, the raw double was stored intact, including sign bit.
                const uint8_t originalType = typeBits->readNumeric();
                uint64_t encoded = readType<uint64_t>(reader, inverted);
                encoded = endian::bigToNative(encoded);
                double d;
                memcpy(&d, &encoded, sizeof(d));

                if (originalType == TypeBits::kDouble) {
                    *stream << d;
                }
                else {
                    // This can only happen for a single number.
                    invariant(originalType == TypeBits::kLong);
                    invariant(d == double(std::numeric_limits<long long>::min()));
                    *stream << std::numeric_limits<long long>::min();
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
                        encodedIntegerPart = (encodedIntegerPart << 8)
                                           | readType<uint8_t>(reader, inverted);
                    }
                }

                const bool haveFractionalPart = (encodedIntegerPart & 1);
                long long integerPart = encodedIntegerPart >> 1;

                if (!haveFractionalPart) {
                    if (isNegative)
                        integerPart = -integerPart;

                    switch(originalType) {
                    case TypeBits::kDouble: *stream << double(integerPart); break;
                    case TypeBits::kInt: *stream << int(integerPart); break;
                    case TypeBits::kLong: *stream << integerPart; break;
                    case TypeBits::kNegativeZero: invariant(false);
                    }
                }
                else {
                    // Nothing else can have a fractional part.
                    invariant(originalType == TypeBits::kDouble);

                    const uint64_t exponent = (64 - countLeadingZeros64(integerPart)) - 1;
                    const size_t fractionalBits = (52 - exponent);
                    const size_t fractionalBytes = (fractionalBits + 7) / 8;

                    // build up the bits of a double here.
                    uint64_t doubleBits = integerPart << fractionalBits;
                    doubleBits &= ~(1ull << 52); // clear implicit leading 1
                    doubleBits |= (exponent + 1023/*bias*/) << 52;
                    if (isNegative) {
                        doubleBits |= (1ull << 63); // sign bit
                    }
                    for (size_t i = 0; i < fractionalBytes; i++) {
                        // fold in the fractional bytes
                        const uint64_t byte = readType<uint8_t>(reader, inverted);
                        doubleBits |= (byte << ((fractionalBytes - i - 1) * 8));
                    }

                    double number;
                    memcpy(&number, &doubleBits, sizeof(number));
                    *stream << number;
                }

                break;
            }
            default: invariant(false);
            }
        }
    } // namespace

    BSONObj KeyString::toBson(const char* buffer, size_t len, Ordering ord,
                              const TypeBits& typeBits) {
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
            toBsonValue(ctype, &reader, &typeBitsReader, invert, &(builder << ""));
        }
        return builder.obj();
    }

    BSONObj KeyString::toBson(StringData data, Ordering ord, const TypeBits& typeBits) {
        return toBson(data.rawData(), data.size(), ord, typeBits);
    }

    RecordId KeyString::decodeRecordIdAtEnd(const void* bufferRaw, size_t bufSize) {
        invariant(bufSize >= 2); // smallest possible encoding of a RecordId.
        const unsigned char* buffer = static_cast<const unsigned char*>(bufferRaw);
        const unsigned char lastByte = *(buffer + bufSize - 1);
        const size_t ridSize = 2 + (lastByte & 0x7); // stored in low 3 bits.
        invariant(bufSize >= ridSize);
        const unsigned char* firstBytePtr = buffer + bufSize - ridSize;
        BufReader reader(firstBytePtr, ridSize);
        return decodeRecordId(&reader);
    }

    RecordId KeyString::decodeRecordId(BufReader* reader) {
        const uint8_t firstByte = readType<uint8_t>(reader, false);
        const uint8_t numExtraBytes = firstByte >> 5; // high 3 bits in firstByte
        uint64_t repr = firstByte & 0x1f; // low 5 bits in firstByte
        for (int i = 0; i < numExtraBytes; i++) {
            repr = (repr << 8) | readType<uint8_t>(reader, false);
        }

        const uint8_t lastByte = readType<uint8_t>(reader, false);
        invariant((lastByte & 0x7) == numExtraBytes);
        repr = (repr << 5) | (lastByte >> 3); // fold in high 5 bits of last byte
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
            _isAllZeros = false; // it wouldn't be encoded like this if it was.

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

        if (oneOrZero == 1) _isAllZeros = false;

        const uint8_t byte = (_curBit / 8) + 1;
        const uint8_t offsetInByte = _curBit % 8;
        if (offsetInByte == 0) {
            setSizeByte(byte);
            _buf[byte] = oneOrZero; // zeros bits 1-7
        }
        else {
            _buf[byte] |= (oneOrZero << offsetInByte);
        }
        
        _curBit++;
    }

    uint8_t KeyString::TypeBits::Reader::readBit() {
        if (_typeBits._isAllZeros) return 0;

        const uint8_t byte = (_curBit / 8) + 1;
        const uint8_t offsetInByte = _curBit % 8;
        _curBit++;

        dassert(byte <= _typeBits.getSizeByte());

        return (_typeBits._buf[byte] & (1 << offsetInByte)) ? 1 : 0;
    }

} // namespace mongo
