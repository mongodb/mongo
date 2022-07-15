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

#pragma once

#include <limits>

#include <absl/hash/hash.h>

#include "mongo/base/static_assert.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

#include <boost/container/flat_set.hpp>

namespace mongo {

namespace sbe::value {
class ValueBuilder;
}

namespace KeyString {

enum class Version : uint8_t { V0 = 0, V1 = 1, kLatestVersion = V1 };

static StringData keyStringVersionToString(Version version) {
    return version == Version::V0 ? "V0" : "V1";
}

static const Ordering ALL_ASCENDING = Ordering::make(BSONObj());

// Encode the size of a RecordId binary string using up to 4 bytes, 7 bits per byte.
// This supports encoding sizes that fit into 28 bits, which largely covers the
// maximum BSON size.
static const int kRecordIdStrEncodedSizeMaxBytes = 4;
MONGO_STATIC_ASSERT(RecordId::kBigStrMaxSize < 1 << (7 * kRecordIdStrEncodedSizeMaxBytes));

/**
 * Encodes info needed to restore the original BSONTypes from a KeyString. They cannot be
 * stored in place since we don't want them to affect the ordering (1 and 1.0 compare as
 * equal).
 */
class TypeBits {
public:
    // See comments in getBuffer() about short/long encoding schemes.
    static const uint8_t kMaxBytesForShortEncoding = 127;
    static const uint8_t kPrefixBytes = 5;
    static const uint8_t kStoredDecimalExponentBits = 6;
    static const uint32_t kStoredDecimalExponentMask = (1U << kStoredDecimalExponentBits) - 1;

    explicit TypeBits(Version version) : version(version) {
        reset();
    }

    TypeBits(const TypeBits& tb)
        : version(tb.version), _curBit(tb._curBit), _isAllZeros(tb._isAllZeros) {
        _buf.reset();
        _buf.appendBuf(tb._buf.buf(), tb._buf.len());
    }

    TypeBits& operator=(const TypeBits& tb);
    TypeBits(TypeBits&&) = default;
    TypeBits& operator=(TypeBits&&) = default;

    /**
     * If there are no bytes remaining, assumes AllZeros. Otherwise, reads bytes out of the
     * BufReader in the format described on the getBuffer() method.
     */
    void resetFromBuffer(BufReader* reader);
    static TypeBits fromBuffer(Version version, BufReader* reader) {
        TypeBits out(version);
        out.resetFromBuffer(reader);
        return out;
    }

    /**
     * If true, no bits have been set to one. This is true if no bits have been set at all.
     */
    bool isAllZeros() const {
        return _isAllZeros;
    }

    /**
     * These methods return a buffer and size which encodes all of the type bits in this
     * instance.
     *
     * Encoded format:
     * Case 1 (first byte is 0x0):
     *     This encodes the "AllZeros" state which represents an infinite stream of bits set
     *     to 0. Callers may optionally encode this case as an empty buffer if they have
     *     another way to mark the end of the buffer. There are no follow-up bytes.
     *
     * Case 2 (first byte isn't 0x0 but has high bit set to 0):
     *     The first byte is the only data byte. This can represent any 7-bit sequence or an
     *     8-bit sequence if the 8th bit is 0, since the 8th bit is the same as the bit that
     *     is 1 if the first byte is the size byte. There are no follow-up bytes.
     *
     * Case 3 (first byte has high bit set to 1 but it's not 0x80):
     *     Remaining bits of first byte encode number of follow-up bytes that are data
     *     bytes.
     *
     * Case 4 (first byte is 0x80)
     *     The first byte is the signal byte indicating that this TypeBits is encoded with long
     *     encoding scheme: the next four bytes (in little endian order) represent the number of
     *     data bytes.
     *
     * Within data bytes (ie everything excluding the size byte if there is one), bits are
     * packed in from low to high.
     */
    const char* getBuffer() const {
        if (_isAllZeros)
            return "";  // Case 1: pointer to a zero byte.

        if (getSize() == 1)
            return getDataBuffer();  // Case 2: all bits in one byte; no size byte.

        // Case 3 & 4: size byte(s) + data bytes.
        return isLongEncoding() ? _buf.buf() : (getDataBuffer() - 1);
    }
    size_t getSize() const {
        if (_isAllZeros) {  // Case 1
            dassert(getDataBufferLen() == 0 || getDataBuffer()[0] == 0);
            return 1;
        }

        uint32_t rawSize = getDataBufferLen();
        dassert(rawSize >= 1);                      // 0 should be handled as isAllZeros.
        if (rawSize > kMaxBytesForShortEncoding) {  // Case 4
            return rawSize + kPrefixBytes;
        }
        if (rawSize == 1 && !(getDataBuffer()[0] & 0x80)) {  // Case 2
            return 1;
        }

        return rawSize + 1;  // Case 3
    }

    bool isLongEncoding() const {
        // TypeBits with all zeros is in short encoding regardless of the data buffer length.
        return !_isAllZeros && getDataBufferLen() > kMaxBytesForShortEncoding;
    }

    //
    // Everything below is only for use by KeyString::Builder.
    //

    // Note: No space is used if all bits are 0 so the most common cases should be 0x0.
    static const uint8_t kString = 0x0;
    static const uint8_t kSymbol = 0x1;

    static const uint8_t kInt = 0x0;
    static const uint8_t kLong = 0x1;
    static const uint8_t kDouble = 0x2;
    static const uint8_t kDecimal = 0x3;               // indicates 6 more bits of typeinfo follow.
    static const uint8_t kSpecialZeroPrefix = 0x3;     // kNumericZero case, 3 more bits follow.
    static const uint8_t kNegativeDoubleZero = 0x3;    // normalized -0.0 double, either V0 or V1.
    static const uint8_t kV0NegativeDoubleZero = 0x3;  // legacy encoding for V0

    // The following describe the initial 5 type bits for kNegativeOrDecimalZero. These bits
    // encode double -0 or a 3-bit prefix (range 0 to 5) of the 15-bit decimal zero type.
    static const uint8_t kV1NegativeDoubleZero = 0x18;  // 0b11000

    static const uint8_t kUnusedEncoding = 0x19;  // 0b11001

    // There are 6 * (1<<12) == 2 * (kMaxBiasedExponent + 1) == 24576 decimal zeros.
    static const uint8_t kDecimalZero0xxx = 0x1a;  // 0b11010 12 more exponent bits follow
    static const uint8_t kDecimalZero1xxx = 0x1b;  // 0b11011
    static const uint8_t kDecimalZero2xxx = 0x1c;  // 0b11100
    static const uint8_t kDecimalZero3xxx = 0x1d;  // 0b11101
    static const uint8_t kDecimalZero4xxx = 0x1e;  // 0b11110
    static const uint8_t kDecimalZero5xxx = 0x1f;  // 0b11111

    void reset() {
        _curBit = 0;
        _isAllZeros = true;
        _buf.setlen(kPrefixBytes);
    }

    void appendString() {
        appendBit(kString);
    }
    void appendSymbol() {
        appendBit(kSymbol);
    }

    void appendNumberDouble() {
        appendBit(kDouble >> 1);
        appendBit(kDouble & 1);
    }
    void appendNumberInt() {
        appendBit(kInt >> 1);
        appendBit(kInt & 1);
    }
    void appendNumberLong() {
        appendBit(kLong >> 1);
        appendBit(kLong & 1);
    }
    void appendNumberDecimal() {
        appendBit(kDecimal >> 1);
        appendBit(kDecimal & 1);
    }
    void appendZero(uint8_t zeroType);
    void appendDecimalZero(uint32_t whichZero);
    void appendDecimalExponent(uint8_t storedExponentBits);

    class Reader {
    public:
        /**
         * Passed in TypeBits must outlive this Reader instance.
         */
        explicit Reader(const TypeBits& typeBits) : _curBit(0), _typeBits(typeBits) {}

        uint8_t readStringLike() {
            return readBit();
        }
        uint8_t readNumeric() {
            uint8_t highBit = readBit();
            return (highBit << 1) | readBit();
        }
        uint8_t readZero();

        // Given a decimal zero type between kDecimalZero0xxx and kDecimal5xxx, read the
        // remaining 12 bits and return which of the 24576 decimal zeros to produce.
        uint32_t readDecimalZero(uint8_t zeroType);

        // Reads the stored exponent bits of a non-zero decimal number.
        uint8_t readDecimalExponent();

    private:
        uint8_t readBit();

        uint32_t _curBit;
        const TypeBits& _typeBits;
    };

    Version version;

private:
    static uint32_t readSizeFromBuffer(BufReader* reader);

    void setRawSize(uint32_t size);

    const char* getDataBuffer() const {
        return _buf.buf() + kPrefixBytes;
    }
    char* getDataBuffer() {
        return _buf.buf() + kPrefixBytes;
    }
    uint32_t getDataBufferLen() const {
        return _buf.len() - kPrefixBytes;
    }

    void appendBit(uint8_t oneOrZero);

    uint32_t _curBit;
    bool _isAllZeros;

    /**
     * See getBuffer()/getSize() documentation for a description of how data is encoded. When
     * the TypeBits size is in short encoding range(<=127), the bytes starting from the fifth
     * byte are the complete TypeBits in short encoding scheme (1 size byte + data bytes).  When
     * the TypeBits size is in long encoding range(>127), all the bytes are used for the long
     * encoding format (first byte + 4 size bytes + data bytes).
     */

    // TypeBits buffers are often small and at least 5 bytes. Only pre-allocate a small amount of
    // memory despite using a StackBufBuilder, which can use cheap stack space. Because TypeBits is
    // allowed to be allocated dynamically on the heap, so is the owned StackBufBuilder. Lower the
    // initial buffer size so that we do not pre-allocate excessively large buffers on the heap when
    // TypeBits is not a stack variable.
    enum { SmallStackSize = 8 };
    StackBufBuilderBase<SmallStackSize> _buf;
};


/**
 * Value owns a buffer that corresponds to a completely generated KeyString::Builder with the
 * TypeBits appended.
 *
 * To optimize copy performance and space requirements of this structure, the buffer will contain
 * the full KeyString with the TypeBits appended at the end.
 */
class Value {

public:
    Value() : _version(Version::kLatestVersion), _ksSize(0) {}

    Value(Version version, int32_t ksSize, SharedBufferFragment buffer)
        : _version(version), _ksSize(ksSize), _buffer(std::move(buffer)) {
        invariant(ksSize >= 0);
        invariant(ksSize <= static_cast<int32_t>(_buffer.size()));
    }

    Value(const Value&) = default;
    Value(Value&&) = default;

    // Use a copy-and-swap, which prevents unnecessary allocation and deallocations.
    Value& operator=(Value copy) noexcept {
        _version = copy._version;
        _ksSize = copy._ksSize;
        std::swap(_buffer, copy._buffer);
        return *this;
    }

    /**
     * Compare with another KeyString::Value or Builder.
     */
    template <class T>
    int compare(const T& other) const;

    int compareWithTypeBits(const Value& other) const;

    /**
     * Compare with another KeyString::Value or Builder, ignoring the RecordId part of both.
     */
    template <class T>
    int compareWithoutRecordIdLong(const T& other) const;
    template <class T>
    int compareWithoutRecordIdStr(const T& other) const;

    // Returns the size of the stored KeyString.
    size_t getSize() const {
        return _ksSize;
    }

    // Returns whether the size of the stored KeyString is 0.
    bool isEmpty() const {
        return _ksSize == 0;
    }

    const char* getBuffer() const {
        return _buffer.get();
    }

    // Returns the stored TypeBits.
    TypeBits getTypeBits() const {
        const char* buf = _buffer.get() + _ksSize;
        BufReader reader(buf, _buffer.size() - _ksSize);
        return TypeBits::fromBuffer(_version, &reader);
    }

    // Compute hash over key
    uint64_t hash(uint64_t seed = 0) const {
        return absl::hash_internal::CityHash64WithSeed(_buffer.get(), _buffer.size(), seed);
    }

    /**
     * Returns a hex encoding of this key.
     */
    std::string toString() const;

    // Serializes this Value into a storable format with TypeBits information. The serialized
    // format takes the following form:
    //   [keystring size][keystring encoding][typebits encoding]
    void serialize(BufBuilder& buf) const {
        buf.appendNum(_ksSize);                        // Serialize size of Keystring
        buf.appendBuf(_buffer.get(), _buffer.size());  // Serialize Keystring + Typebits
    }

    /**
     * Serializes this Value, excluding the RecordId, into a storable format with TypeBits
     * information. The serialized format takes the following form:
     *   [keystring size][keystring encoding][typebits encoding]
     */
    void serializeWithoutRecordIdLong(BufBuilder& buf) const;
    void serializeWithoutRecordIdStr(BufBuilder& buf) const;

    // Deserialize the Value from a serialized format.
    static Value deserialize(BufReader& buf, KeyString::Version version) {
        const int32_t sizeOfKeystring = buf.read<LittleEndian<int32_t>>();
        const void* keystringPtr = buf.skip(sizeOfKeystring);

        BufBuilder newBuf;
        newBuf.appendBuf(keystringPtr, sizeOfKeystring);

        auto typeBits = TypeBits::fromBuffer(version, &buf);  // advances the buf
        if (typeBits.isAllZeros()) {
            newBuf.appendChar(0);
        } else {
            newBuf.appendBuf(typeBits.getBuffer(), typeBits.getSize());
        }
        // Note: this variable is needed to make sure that no method is called on 'newBuf'
        // after a call on its 'release' method.
        const size_t newBufLen = newBuf.len();
        return {version, sizeOfKeystring, SharedBufferFragment(newBuf.release(), newBufLen)};
    }

    /// Members for Sorter
    struct SorterDeserializeSettings {
        SorterDeserializeSettings(Version version) : keyStringVersion(version) {}
        Version keyStringVersion;
    };

    void serializeForSorter(BufBuilder& buf) const {
        serialize(buf);
    }

    static Value deserializeForSorter(BufReader& buf, const SorterDeserializeSettings& settings) {
        return deserialize(buf, settings.keyStringVersion);
    }

    int memUsageForSorter() const {
        // Ideally we want to always use the buffer capacity as a more accurate measure of memory
        // usage here. But when built using the PooledBuilder we cannot do that as the buffer is
        // shared between many instances we have to use the size() as an approximation of memory
        // use. There might be a chunk at the end of the buffer that's not used by any KeyString and
        // that memory will be unaccounted for unfortunately.
        // When the PooledBuilder is used this buffer will always be shared as the
        // SharedBufferFragmentBuilder will keep a reference. If it is not shared we've used either
        // the Heap or Static builder and want to report the whole memory allocation in the buffer.
        return sizeof(Value) + (_buffer.isShared() ? _buffer.size() : _buffer.underlyingCapacity());
    }

    Value getOwned() const {
        return *this;
    }

    Version getVersion() const {
        return _version;
    }

    size_t getApproximateSize() const;

private:
    Version _version;
    // _ksSize is the total length that the KeyString takes up in the buffer.
    int32_t _ksSize;
    SharedBufferFragment _buffer;
};

enum class Discriminator {
    kInclusive,  // Anything to be stored in an index must use this.
    kExclusiveBefore,
    kExclusiveAfter,
};

enum class BuildState {
    kEmpty,                  // Buffer is empty.
    kAppendingBSONElements,  // In the process of appending BSON Elements
    kEndAdded,               // Finished appedning BSON Elements.
    kAppendedRecordID,       // Finished appending a RecordID.
    kAppendedTypeBits,       // Finished appending a TypeBits.
    kReleased                // Released the buffer and so the buffer is no longer valid.
};

/**
 * Encodes the kind of NumberDecimal that is stored.
 */
enum DecimalContinuationMarker {
    kDCMEqualToDouble = 0x0,
    kDCMHasContinuationLessThanDoubleRoundedUpTo15Digits = 0x1,
    kDCMEqualToDoubleRoundedUpTo15Digits = 0x2,
    kDCMHasContinuationLargerThanDoubleRoundedUpTo15Digits = 0x3
};

using StringTransformFn = std::function<std::string(StringData)>;

template <class BuilderT>
class BuilderBase {
public:
    BuilderBase(Version version, Ordering ord, Discriminator discriminator)
        : version(version),
          _typeBits(version),
          _state(BuildState::kEmpty),
          _elemCount(0),
          _ordering(ord),
          _discriminator(discriminator) {}

    BuilderBase(Version version, Ordering ord)
        : BuilderBase(version, ord, Discriminator::kInclusive) {}
    explicit BuilderBase(Version version)
        : BuilderBase(version, ALL_ASCENDING, Discriminator::kInclusive) {}

    BuilderBase(Version version, const BSONObj& obj, Ordering ord, const RecordId& recordId)
        : BuilderBase(version, ord) {
        resetToKey(obj, ord, recordId);
    }

    BuilderBase(Version version,
                const BSONObj& obj,
                Ordering ord,
                Discriminator discriminator = Discriminator::kInclusive)
        : BuilderBase(version, ord) {
        resetToKey(obj, ord, discriminator);
    }

    BuilderBase(const BuilderBase& other)
        : version(other.version),
          _typeBits(other.getTypeBits()),
          _state(other._state),
          _elemCount(other._elemCount),
          _ordering(other._ordering),
          _discriminator(other._discriminator) {
        resetFromBuffer(other.getBuffer(), other.getSize());
    }

    BuilderBase(Version version, const RecordId& rid) : BuilderBase(version) {
        appendRecordId(rid);
    }

    /**
     * Copies the data held in this buffer into a Value type that holds and owns a copy of the
     * buffer.
     */
    Value getValueCopy() {
        _doneAppending();

        // Create a new buffer that is a concatenation of the KeyString and its TypeBits.
        BufBuilder newBuf(_buffer().len() + _typeBits.getSize());
        newBuf.appendBuf(_buffer().buf(), _buffer().len());
        if (_typeBits.isAllZeros()) {
            newBuf.appendChar(0);
        } else {
            newBuf.appendBuf(_typeBits.getBuffer(), _typeBits.getSize());
        }
        // Note: this variable is needed to make sure that no method is called on 'newBuf'
        // after a call on its 'release' method.
        const size_t newBufLen = newBuf.len();
        return {version, _buffer().len(), SharedBufferFragment(newBuf.release(), newBufLen)};
    }

    void appendRecordId(const RecordId& loc);
    void appendTypeBits(const TypeBits& bits);

    /*
     * Function 'f' will be applied to all string elements contained in 'elem'.
     */
    void appendBSONElement(const BSONElement& elem, const StringTransformFn& f = nullptr);

    void appendBool(bool val);
    void appendString(StringData val, const StringTransformFn& f = nullptr);
    void appendSymbol(StringData val);
    void appendNumberDouble(double num);
    void appendNumberLong(long long num);
    void appendNumberInt(int num);
    void appendNumberDecimal(Decimal128 num);
    void appendNull();
    void appendUndefined();
    void appendCodeWString(const BSONCodeWScope& val);
    void appendBinData(const BSONBinData& data);
    void appendRegex(const BSONRegEx& val);
    void appendSetAsArray(const BSONElementSet& set, const StringTransformFn& f = nullptr);
    void appendOID(OID oid);
    void appendDate(Date_t date);
    void appendTimestamp(Timestamp val);
    void appendBytes(const void* source, size_t bytes);
    void appendDBRef(const BSONDBRef& val);
    void appendObject(const BSONObj& val, const StringTransformFn& f = nullptr);
    void appendArray(const BSONArray& val, const StringTransformFn& f = nullptr);
    void appendCode(StringData val);

    /**
     * Appends a Discriminator byte and kEnd byte to a key string.
     */
    void appendDiscriminator(Discriminator discriminator);

    /**
     * Resets to an empty state.
     * Equivalent to but faster than *this = Builder(ord, discriminator)
     */
    void resetToEmpty(Ordering ord = ALL_ASCENDING,
                      Discriminator discriminator = Discriminator::kInclusive) {
        _reinstantiateBufferIfNeeded();
        _buffer().reset();
        _typeBits.reset();

        _elemCount = 0;
        _ordering = ord;
        _discriminator = discriminator;
        _transition(BuildState::kEmpty);
    }

    void resetToKey(const BSONObj& obj, Ordering ord, const RecordId& recordId);
    void resetToKey(const BSONObj& obj,
                    Ordering ord,
                    Discriminator discriminator = Discriminator::kInclusive);
    void resetFromBuffer(const void* buffer, size_t size) {
        _buffer().reset();
        memcpy(_buffer().skip(size), buffer, size);
    }

    const char* getBuffer() const {
        invariant(_state != BuildState::kReleased);
        return _buffer().buf();
    }

    size_t getSize() const {
        invariant(_state != BuildState::kReleased);
        return _buffer().len();
    }

    bool isEmpty() const {
        invariant(_state != BuildState::kReleased);
        return _buffer().len() == 0;
    }

    void setTypeBits(const TypeBits& typeBits) {
        invariant(_state != BuildState::kReleased);
        _typeBits = typeBits;
    }

    const TypeBits& getTypeBits() const {
        invariant(_state != BuildState::kReleased);
        return _typeBits;
    }

    /**
     * Compare with another KeyString::Value or Builder.
     */
    template <class T>
    int compare(const T& other) const;

    /**
     * Compare with another KeyString::Value or Builder, ignoring the RecordId part of both.
     */
    template <class T>
    int compareWithoutRecordIdLong(const T& other) const;
    template <class T>
    int compareWithoutRecordIdStr(const T& other) const;

    /**
     * @return a hex encoding of this key
     */
    std::string toString() const;

    /**
     * Version to use for conversion to/from KeyString. V1 has different encodings for numeric
     * values.
     */
    const Version version;

protected:
    void _appendAllElementsForIndexing(const BSONObj& obj, Discriminator discriminator);

    void _appendBool(bool val, bool invert);
    void _appendDate(Date_t val, bool invert);
    void _appendTimestamp(Timestamp val, bool invert);
    void _appendOID(OID val, bool invert);
    void _appendString(StringData val, bool invert, const StringTransformFn& f);
    void _appendSymbol(StringData val, bool invert);
    void _appendCode(StringData val, bool invert);
    void _appendCodeWString(const BSONCodeWScope& val, bool invert);
    void _appendBinData(const BSONBinData& val, bool invert);
    void _appendRegex(const BSONRegEx& val, bool invert);
    void _appendDBRef(const BSONDBRef& val, bool invert);
    void _appendArray(const BSONArray& val, bool invert, const StringTransformFn& f);
    void _appendSetAsArray(const BSONElementSet& val, bool invert, const StringTransformFn& f);
    void _appendObject(const BSONObj& val, bool invert, const StringTransformFn& f);
    void _appendNumberDouble(double num, bool invert);
    void _appendNumberLong(long long num, bool invert);
    void _appendNumberInt(int num, bool invert);
    void _appendNumberDecimal(Decimal128 num, bool invert);

    void _appendRecordIdLong(int64_t val);
    void _appendRecordIdStr(const char* val, int size);

    /**
     * @param name - optional, can be NULL
     *              if NULL, not included in encoding
     *              if not NULL, put in after type, before value
     */
    void _appendBsonValue(const BSONElement& elem,
                          bool invert,
                          const StringData* name,
                          const StringTransformFn& f);

    void _appendStringLike(StringData str, bool invert);
    void _appendBson(const BSONObj& obj, bool invert, const StringTransformFn& f);
    void _appendSmallDouble(double value, DecimalContinuationMarker dcm, bool invert);
    void _appendLargeDouble(double value, DecimalContinuationMarker dcm, bool invert);
    void _appendInteger(long long num, bool invert);
    void _appendPreshiftedIntegerPortion(uint64_t value, bool isNegative, bool invert);

    void _appendDoubleWithoutTypeBits(double num, DecimalContinuationMarker dcm, bool invert);
    void _appendHugeDecimalWithoutTypeBits(Decimal128 dec, bool invert);
    void _appendTinyDecimalWithoutTypeBits(Decimal128 dec, double bin, bool invert);
    void _appendEnd();

    template <typename T>
    void _append(const T& thing, bool invert) {
        _appendBytes(&thing, sizeof(thing), invert);
    }

    void _appendBytes(const void* source, size_t bytes, bool invert);

    void _doneAppending() {
        if (_state == BuildState::kAppendingBSONElements) {
            appendDiscriminator(_discriminator);
        }
    }

    void _verifyAppendingState() {
        invariant(_state == BuildState::kEmpty || _state == BuildState::kAppendingBSONElements);

        if (_state == BuildState::kEmpty) {
            _transition(BuildState::kAppendingBSONElements);
        }
    }

    void _transition(BuildState to) {
        // We can empty at any point since it just means that we are clearing the buffer.
        if (to == BuildState::kEmpty) {
            _state = to;
            return;
        }

        switch (_state) {
            case BuildState::kEmpty:
                invariant(to == BuildState::kAppendingBSONElements || to == BuildState::kEndAdded ||
                          to == BuildState::kAppendedRecordID);
                break;
            case BuildState::kAppendingBSONElements:
                invariant(to == BuildState::kEndAdded);
                break;
            case BuildState::kEndAdded:
                invariant(to == BuildState::kAppendedRecordID || to == BuildState::kReleased);
                break;
            case BuildState::kAppendedRecordID:
                invariant(to == BuildState::kAppendedTypeBits || to == BuildState::kReleased ||
                          to == BuildState::kAppendedRecordID);
                break;
            case BuildState::kAppendedTypeBits:
                invariant(to == BuildState::kAppendedRecordID || to == BuildState::kReleased);
                break;
            case BuildState::kReleased:
                invariant(to == BuildState::kEmpty);
                break;
            default:
                MONGO_UNREACHABLE;
        }
        _state = to;
    }

    bool _shouldInvertOnAppend() const {
        return _ordering.get(_elemCount) == -1;
    }

    // Appends the TypeBits buffer to the main buffer and returns the offset of where the TypeBits
    // begin
    int32_t _appendTypeBits() {
        _doneAppending();

        // append the TypeBits.
        int32_t ksSize = _buffer().len();
        if (_typeBits.isAllZeros()) {
            _buffer().appendChar(0);
        } else {
            _buffer().appendBuf(_typeBits.getBuffer(), _typeBits.getSize());
        }
        return ksSize;
    }

    auto& _buffer() {
        return static_cast<BuilderT*>(this)->_buffer();
    }

    const auto& _buffer() const {
        return static_cast<const BuilderT*>(this)->_buffer();
    }

    void _reinstantiateBufferIfNeeded() {
        static_cast<BuilderT*>(this)->_reinstantiateBufferIfNeeded();
    }

    TypeBits _typeBits;
    BuildState _state;
    int _elemCount;
    Ordering _ordering;
    Discriminator _discriminator;
};

// Helper class to hold a buffer builder. This class needs to be before BuilderBase when inheriting
// to ensure the buffer is constructed first
template <typename BufferBuilderT>
class BufferHolder {
protected:
    template <typename... Args>
    BufferHolder(Args&&... args) : _bufferBuilder(std::forward<Args>(args)...) {}
    BufferBuilderT _bufferBuilder;
};

class Builder : private BufferHolder<StackBufBuilder>, public BuilderBase<Builder> {
public:
    using BuilderBase::BuilderBase;

    Builder(const Builder& other) : BuilderBase(other) {}

public:
    friend class BuilderBase;

    StackBufBuilder& _buffer() {
        return _bufferBuilder;
    }
    const StackBufBuilder& _buffer() const {
        return _bufferBuilder;
    }

    void _reinstantiateBufferIfNeeded() {}
};
class HeapBuilder : private BufferHolder<BufBuilder>, public BuilderBase<HeapBuilder> {
public:
    static constexpr uint8_t kHeapAllocatorDefaultBytes = 32;

    // Forwarding constructor to BuilderBase
    template <typename... Args>
    HeapBuilder(Args&&... args)
        : BufferHolder(kHeapAllocatorDefaultBytes), BuilderBase(std::forward<Args>(args)...) {}

    // When copying don't allocate memory by default. Copy-constructor will request the right amount
    // of memory
    HeapBuilder(const HeapBuilder& other) : BufferHolder(0), BuilderBase(other) {}

    /**
     * Releases the data held in this buffer into a Value type, releasing and transfering ownership
     * of the buffer _buffer and TypeBits _typeBits to the returned Value object from the current
     * Builder.
     */
    Value release() {
        int32_t ksSize = _appendTypeBits();
        _transition(BuildState::kReleased);

        // Note: this variable is needed to make sure that no method is called on '_bufferBuilder'
        // after a call on its 'release' method.
        const size_t bufLen = _bufferBuilder.len();
        return {version, ksSize, SharedBufferFragment(_bufferBuilder.release(), bufLen)};
    }

protected:
    friend class BuilderBase;

    BufBuilder& _buffer() {
        return _bufferBuilder;
    }
    const BufBuilder& _buffer() const {
        return _bufferBuilder;
    }

    void _reinstantiateBufferIfNeeded() {
        if (_state == BuildState::kReleased) {
            _bufferBuilder = BufBuilder(kHeapAllocatorDefaultBytes);
        }
    }
};
class PooledBuilder : private BufferHolder<PooledFragmentBuilder>,
                      public BuilderBase<PooledBuilder> {
public:
    template <typename... Args>
    PooledBuilder(SharedBufferFragmentBuilder& memoryPool, Args&&... args)
        : BufferHolder(memoryPool), BuilderBase(std::forward<Args>(args)...) {}

    // Underlying SharedBufferFragmentBuilder can only build one buffer at the time, so copy does
    // not work for the PooledBuilder.
    PooledBuilder(const PooledBuilder&) = delete;

    Value release() {
        int32_t ksSize = _appendTypeBits();
        _transition(BuildState::kReleased);
        return {version, ksSize, _bufferBuilder.done()};
    }

public:
    friend class BuilderBase;

    PooledFragmentBuilder& _buffer() {
        return _bufferBuilder;
    }
    const PooledFragmentBuilder& _buffer() const {
        return _bufferBuilder;
    }

    void _reinstantiateBufferIfNeeded() {}
};

/*
 * The isKeyString struct allows the operators below to only be enabled if the types being operated
 * on are KeyStrings.
 */
template <class T>
struct isKeyString : public std::false_type {};

template <>
struct isKeyString<Builder> : public std::true_type {};
template <>
struct isKeyString<HeapBuilder> : public std::true_type {};
template <>
struct isKeyString<PooledBuilder> : public std::true_type {};

template <>
struct isKeyString<Value> : public std::true_type {};

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator<(const T& lhs,
                                                                            const U& rhs) {
    return lhs.compare(rhs) < 0;
}

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator<=(const T& lhs,
                                                                             const U& rhs) {
    return lhs.compare(rhs) <= 0;
}

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator==(const T& lhs,
                                                                             const U& rhs) {
    return lhs.compare(rhs) == 0;
}

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator>(const T& lhs,
                                                                            const U& rhs) {
    return lhs.compare(rhs) > 0;
}

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator>=(const T& lhs,
                                                                             const U& rhs) {
    return lhs.compare(rhs) >= 0;
}

template <class T, class U>
inline typename std::enable_if<isKeyString<T>::value, bool>::type operator!=(const T& lhs,
                                                                             const U& rhs) {
    return !(lhs == rhs);
}

template <class T>
inline typename std::enable_if<isKeyString<T>::value, std::ostream&>::type operator<<(
    std::ostream& stream, const T& value) {
    return stream << value.toString();
}

/**
 * Given a KeyString which may or may not have a RecordId, returns the length of the section without
 * the RecordId. More expensive than sizeWithoutRecordId(Long|Str)AtEnd
 */
size_t getKeySize(const char* buffer, size_t len, Ordering ord, const TypeBits& typeBits);

/**
 * Decodes the given KeyString buffer into it's BSONObj representation. This is marked as
 * noexcept since the assumption is that 'buffer' is a valid KeyString buffer and this method
 * is not expected to throw.
 *
 * If the buffer provided may not be valid, use the 'safe' version instead.
 */
BSONObj toBson(StringData data, Ordering ord, const TypeBits& types);
BSONObj toBson(const char* buffer, size_t len, Ordering ord, const TypeBits& types) noexcept;
BSONObj toBsonSafe(const char* buffer, size_t len, Ordering ord, const TypeBits& types);
void toBsonSafe(
    const char* buffer, size_t len, Ordering ord, const TypeBits& types, BSONObjBuilder& builder);
Discriminator decodeDiscriminator(const char* buffer,
                                  size_t len,
                                  Ordering ord,
                                  const TypeBits& typeBits);

template <class T>
BSONObj toBson(const T& keyString, Ordering ord) noexcept {
    return toBson(keyString.getBuffer(), keyString.getSize(), ord, keyString.getTypeBits());
}

/**
 * Decodes a RecordId long from the end of a buffer.
 */
RecordId decodeRecordIdLongAtEnd(const void* buf, size_t size);

/**
 * Decodes a RecordId string from the end of a buffer.
 * The RecordId string length cannot be determined by looking at the start of the string.
 */
RecordId decodeRecordIdStrAtEnd(const void* buf, size_t size);

/**
 * Given a KeyString with a RecordId in the long format, returns the length of the section without
 * the RecordId.
 */
size_t sizeWithoutRecordIdLongAtEnd(const void* bufferRaw, size_t bufSize);

/**
 * Given a KeyString with a RecordId in the string format, returns the length of the section without
 * the RecordId.
 */
size_t sizeWithoutRecordIdStrAtEnd(const void* bufferRaw, size_t bufSize);

/**
 * Decodes a RecordId, consuming all bytes needed from reader.
 */
RecordId decodeRecordIdLong(BufReader* reader);

int compare(const char* leftBuf, const char* rightBuf, size_t leftSize, size_t rightSize);

/**
 * Read one KeyString component from the given 'reader' and 'typeBits' inputs and stream it to the
 * 'valueBuilder' object, which converts it to a "Slot-Based Execution" (SBE) representation. When
 * no components remain in the KeyString, this function returns false and leaves 'valueBuilder'
 * unmodified.
 */
bool readSBEValue(BufReader* reader,
                  TypeBits::Reader* typeBits,
                  bool inverted,
                  Version version,
                  sbe::value::ValueBuilder* valueBuilder);

/*
 * Appends the first field of a key string to a BSON object.
 * This does not accept TypeBits because callers of this function discard TypeBits.
 */
void appendSingleFieldToBSONAs(const char* buf,
                               int len,
                               StringData fieldName,
                               BSONObjBuilder* builder,
                               Version version = KeyString::Version::kLatestVersion);

template <class BufferT>
template <class T>
int BuilderBase<BufferT>::compare(const T& other) const {
    return KeyString::compare(getBuffer(), other.getBuffer(), getSize(), other.getSize());
}

template <class BufferT>
template <class T>
int BuilderBase<BufferT>::compareWithoutRecordIdLong(const T& other) const {
    return KeyString::compare(
        getBuffer(),
        other.getBuffer(),
        !isEmpty() ? sizeWithoutRecordIdLongAtEnd(getBuffer(), getSize()) : 0,
        !other.isEmpty() ? sizeWithoutRecordIdLongAtEnd(other.getBuffer(), other.getSize()) : 0);
}

template <class BufferT>
template <class T>
int BuilderBase<BufferT>::compareWithoutRecordIdStr(const T& other) const {
    return KeyString::compare(
        getBuffer(),
        other.getBuffer(),
        !isEmpty() ? sizeWithoutRecordIdStrAtEnd(getBuffer(), getSize()) : 0,
        !other.isEmpty() ? sizeWithoutRecordIdStrAtEnd(other.getBuffer(), other.getSize()) : 0);
}

template <class T>
int Value::compare(const T& other) const {
    return KeyString::compare(getBuffer(), other.getBuffer(), getSize(), other.getSize());
}

template <class T>
int Value::compareWithoutRecordIdLong(const T& other) const {
    return KeyString::compare(
        getBuffer(),
        other.getBuffer(),
        !isEmpty() ? sizeWithoutRecordIdLongAtEnd(getBuffer(), getSize()) : 0,
        !other.isEmpty() ? sizeWithoutRecordIdLongAtEnd(other.getBuffer(), other.getSize()) : 0);
}

template <class T>
int Value::compareWithoutRecordIdStr(const T& other) const {
    return KeyString::compare(
        getBuffer(),
        other.getBuffer(),
        !isEmpty() ? sizeWithoutRecordIdStrAtEnd(getBuffer(), getSize()) : 0,
        !other.isEmpty() ? sizeWithoutRecordIdStrAtEnd(other.getBuffer(), other.getSize()) : 0);
}

/**
 * Takes key string and key pattern information and uses it to present human-readable information
 * about an index or collection entry.
 *
 * 'logPrefix' addes a logging prefix. Useful for differentiating callers.
 */
void logKeyString(const RecordId& recordId,
                  const Value& keyStringValue,
                  const BSONObj& keyPatternBson,
                  const BSONObj& keyStringBson,
                  std::string callerLogPrefix);

}  // namespace KeyString

using KeyStringSet = boost::container::flat_set<KeyString::Value>;

}  // namespace mongo
