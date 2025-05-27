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

#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/hash/hash.h>
#include <boost/container/flat_set.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace key_string {
class BuilderInterface {
public:
    virtual ~BuilderInterface() = default;

    virtual void append(const MinKeyLabeler& id) = 0;
    virtual void append(const MaxKeyLabeler& id) = 0;
    virtual void append(const NullLabeler& id) = 0;
    virtual void append(const UndefinedLabeler& id) = 0;
    virtual void append(bool in) = 0;
    virtual void append(const Date_t& in) = 0;
    virtual void append(const Timestamp& in) = 0;
    virtual void append(const OID& in) = 0;
    virtual void append(const std::string& in) = 0;
    virtual void append(StringData in) = 0;
    virtual void append(const BSONSymbol& in) = 0;
    virtual void append(const BSONCode& in) = 0;
    virtual void append(const BSONCodeWScope& in) = 0;
    virtual void append(const BSONBinData& in) = 0;
    virtual void append(const BSONRegEx& in) = 0;
    virtual void append(const BSONDBRef& in) = 0;
    virtual void append(double in) = 0;
    virtual void append(const Decimal128& in) = 0;
    virtual void append(long long in) = 0;
    virtual void append(int32_t in) = 0;

    template <typename T>
    BuilderInterface& operator<<(const T& t) {
        append(t);
        return *this;
    }

    virtual BufBuilder& subobjStart() = 0;
    virtual BufBuilder& subarrayStart() = 0;
};

enum class Version : uint8_t { V0 = 0, V1 = 1, kLatestVersion = V1 };

inline StringData keyStringVersionToString(Version version) {
    return version == Version::V0 ? "V0" : "V1";
}

inline const Ordering ALL_ASCENDING = Ordering::allAscending();

// Encode the size of a RecordId binary string using up to 4 bytes, 7 bits per byte.
// This supports encoding sizes that fit into 28 bits, which largely covers the
// maximum BSON size.
inline constexpr int kRecordIdStrEncodedSizeMaxBytes = 4;
MONGO_STATIC_ASSERT(RecordId::kBigStrMaxSize < 1 << (7 * kRecordIdStrEncodedSizeMaxBytes));

/**
 * Encodes info needed to restore the original BSONTypes from a KeyString. They cannot be
 * stored in place since we don't want them to affect the ordering (1 and 1.0 compare as
 * equal).
 */
class TypeBits {
public:
    // See comments in getBuffer() about short/long encoding schemes.
    static const int8_t kMaxBytesForShortEncoding = 127;
    static const int8_t kPrefixBytes = 5;
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
    int32_t getSize() const {
        if (_isAllZeros) {  // Case 1
            dassert(getDataBufferLen() == 0 || getDataBuffer()[0] == 0);
            return 1;
        }

        int32_t rawSize = getDataBufferLen();
        dassert(rawSize >= 1);                      // 0 should be handled as isAllZeros.
        if (rawSize > kMaxBytesForShortEncoding) {  // Case 4
            return rawSize + kPrefixBytes;
        }
        if (rawSize == 1 && !(getDataBuffer()[0] & 0x80)) {  // Case 2
            return 1;
        }

        return rawSize + 1;  // Case 3
    }

    /**
     * Gets the buffer as a span. See getBuffer() for information on how the buffer is encoded.
     */
    std::span<const char> getView() const {
        return {getBuffer(), static_cast<size_t>(getSize())};
    }

    bool isLongEncoding() const {
        // TypeBits with all zeros is in short encoding regardless of the data buffer length.
        return !_isAllZeros && getDataBufferLen() > kMaxBytesForShortEncoding;
    }

    //
    // Everything below is only for use by key_string::Builder.
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

    class ReaderBase {
    public:
        virtual ~ReaderBase() {};

        virtual uint8_t readStringLike() = 0;
        virtual uint8_t readNumeric() = 0;
        virtual uint8_t readZero() = 0;
        virtual Version version() const = 0;

        // Given a decimal zero type between kDecimalZero0xxx and kDecimal5xxx, read the
        // remaining 12 bits and return which of the 24576 decimal zeros to produce.
        virtual uint32_t readDecimalZero(uint8_t zeroType) = 0;

        // Reads the stored exponent bits of a non-zero decimal number.
        virtual uint8_t readDecimalExponent() = 0;

    protected:
        ReaderBase() = default;
        virtual uint8_t readBit() = 0;
    };

    class Reader : public ReaderBase {
    public:
        /**
         * Passed in TypeBits must outlive this Reader instance.
         */
        explicit Reader(const char* data, int32_t size, Version version, bool isAllZeroes)
            : _data(data), _size(size), _curBit(0), _version(version), _isAllZeros(isAllZeroes) {}
        explicit Reader(const TypeBits& typeBits)
            : Reader(typeBits.getDataBuffer(),
                     typeBits.getDataBufferLen(),
                     typeBits.version,
                     typeBits._isAllZeros) {}
        ~Reader() override = default;

        uint8_t readStringLike() final;
        uint8_t readNumeric() final;
        uint8_t readZero() final;
        uint32_t readDecimalZero(uint8_t zeroType) final;
        uint8_t readDecimalExponent() final;

        Version version() const override {
            return _version;
        }

    protected:
        uint8_t readBit() final;

    private:
        const char* _data;
        int32_t _size;
        int32_t _curBit;
        Version _version;
        bool _isAllZeros;
    };

    /**
     * An ExplainReader wraps a TypeBits::Reader and stores a human-readable description an about
     * the TypeBits that have been retrieved. The explanation may be retrieved with getExplain().

     * Note that this class is only designed to generate an explanation for a single field. To
     * generate explanations for multiple fields, use multiple ExplainReaders.
     *
     * For diagnostic purposes only.
     */
    class ExplainReader : public ReaderBase {
    public:
        explicit ExplainReader(ReaderBase& reader) : _reader(reader) {};
        ~ExplainReader() override = default;

        uint8_t readStringLike() final;
        uint8_t readNumeric() final;
        uint8_t readZero() final;
        uint32_t readDecimalZero(uint8_t zeroType) final;
        uint8_t readDecimalExponent() final;

        Version version() const final {
            return _reader.version();
        }

        std::string getExplain() const {
            return _explain.ss.str();
        }

    protected:
        uint8_t readBit() final {
            MONGO_UNREACHABLE;
        }

    private:
        ReaderBase& _reader;
        str::stream _explain;
    };

    /**
     * Get a Reader on top of a buffer without copying it. This reader can then be passed to
     * toBson() in place of a TypeBits object that owns a buffer copy.
     * The position pointer of `buf` is advanced to the end of the TypeBits.
     */
    static Reader getReaderFromBuffer(Version version, BufReader* buf) {
        if (!buf->remaining()) {
            // This means AllZeros state was encoded as an empty buffer.
            return Reader(nullptr, 0, version, true);
        }

        int32_t size = readSizeFromBuffer(buf);
        return Reader(static_cast<const char*>(buf->skip(size)), size, version, size == 0);
    }

    Version version;

private:
    static int32_t readSizeFromBuffer(BufReader* reader);

    void setRawSize(int32_t size);

    const char* getDataBuffer() const {
        return _buf.buf() + kPrefixBytes;
    }
    char* getDataBuffer() {
        return _buf.buf() + kPrefixBytes;
    }
    int32_t getDataBufferLen() const {
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
 * A compact type which fits a keystring version and a record id size into four bytes.
 */
class VersionAndSize {
public:
    constexpr VersionAndSize() = default;
    constexpr VersionAndSize(Version version, int32_t size) : _value(_encode(version, size)) {}

    constexpr Version version() const {
        return (_value & (1 << 31)) ? Version::V1 : Version::V0;
    }

    constexpr int32_t size() const {
        return static_cast<int32_t>(_value & ~(1 << 31));
    }

private:
    // The high bit is the encoded version, and the lower 32 bits store the size.
    uint32_t _value = _encode(Version::kLatestVersion, 0);

    static constexpr uint32_t _encode(Version version, int32_t ridSize) {
        uint32_t versionBit = (version == Version::V1) ? 1 << 31 : 0;
        return versionBit | ridSize;
    }
};

/**
 * Value owns a buffer that corresponds to a completely generated key_string::Builder with the
 * TypeBits appended.
 *
 * To optimize copy performance and space requirements of this structure, the buffer will contain
 * the full KeyString with the TypeBits appended at the end.
 */
class Value {
public:
    Value() = default;

    Value(Version version,
          // The size of the KeyString including the RecordId part (if present)
          int32_t ksSize,
          // The size of the RecordId part, if the size is known;
          // or 0, if the RecordId is not present.
          int32_t ridSize,
          SharedBufferFragment buffer)
        : _versionAndRidSize(version, ridSize), _ksSize(ksSize), _buffer(std::move(buffer)) {
        invariant(ridSize >= 0);
        invariant(ksSize >= ridSize);
        invariant(ksSize <= static_cast<int32_t>(_buffer.size()));
    }

    Value(const Value&) = default;
    Value(Value&&) = default;

    // Use a copy-and-swap, which prevents unnecessary allocation and deallocations.
    Value& operator=(Value copy) noexcept {
        _versionAndRidSize = copy._versionAndRidSize;
        _ksSize = copy._ksSize;
        std::swap(_buffer, copy._buffer);
        return *this;
    }

    static Value makeValue(Version version,
                           std::span<const char> ks,
                           std::span<const char> rid,
                           std::span<const char> typeBits);

    /**
     * Compare with another key_string::Value or Builder.
     */
    template <class T>
    int compare(const T& other) const;

    /**
     * Compare with another key_string::Value or Builder, including the TypeBits in the comparison.
     */
    int compareWithTypeBits(const Value& other) const;

    /**
     * Compare with another key_string::Value or Builder, ignoring the RecordId part of both.
     */
    template <class T>
    int compareWithoutRecordIdLong(const T& other) const;
    template <class T>
    int compareWithoutRecordIdStr(const T& other) const;
    template <class T>
    int compareWithoutRecordId(const T& other, KeyFormat keyFormat) const;

    /**
     * Compare with another key_string::Value, ignoring the Discriminator byte of both.
     */
    int compareWithoutDiscriminator(const Value& other) const;

    // Returns the size of the stored KeyString.
    int32_t getSize() const {
        return _ksSize;
    }

    // Returns the size of the RecordId part if present, or 0 otherwise.
    int32_t getRecordIdSize() const {
        return _versionAndRidSize.size();
    }

    // Returns the size of the KeyString without RecordId.
    int32_t getSizeWithoutRecordId() const {
        return _ksSize - getRecordIdSize();
    }

    // Returns whether the size of the stored KeyString is 0.
    bool isEmpty() const {
        return _ksSize == 0;
    }

    // Get the span containing the keystring and record id, but not typebits
    std::span<const char> getView() const {
        return std::span<const char>(_buffer.get(), _ksSize);
    }
    // Get the span containing all three components
    std::span<const char> getViewWithTypeBits() const {
        return std::span<const char>(_buffer.get(), _buffer.size());
    }
    // Get the span containing just the keystring
    std::span<const char> getViewWithoutRecordId() const {
        return getView().first(getSizeWithoutRecordId());
    }
    // Get the span containing just the record id
    std::span<const char> getRecordIdView() const {
        return getView().last(getRecordIdSize());
    }
    // Get the span containing just the typebits
    std::span<const char> getTypeBitsView() const {
        return getViewWithTypeBits().subspan(_ksSize);
    }
    // Get the span containing the record id and typebits
    std::span<const char> getRecordIdAndTypeBitsView() const {
        return getViewWithTypeBits().subspan(getSizeWithoutRecordId());
    }

    // Returns the stored TypeBits.
    TypeBits getTypeBits() const {
        auto view = getTypeBitsView();
        BufReader reader(view.data(), view.size());
        return TypeBits::fromBuffer(getVersion(), &reader);
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
        buf.appendNum(_ksSize);                                // Serialize size of Keystring
        buf.appendBuf(_buffer.get(), _buffer.size());          // Serialize Keystring + Typebits
        if (_buffer.size() == static_cast<size_t>(_ksSize)) {  // Serialize AllZeroes Typebits
            buf.appendChar(0);
        }
    }

    // Deserialize the Value from a serialized format.
    // The caller must pass an ridFormat the indicates the RecordId encoding format, or boost::none
    // if not present.
    static Value deserialize(BufReader& buf,
                             key_string::Version version,
                             boost::optional<KeyFormat> ridFormat);

    /// Members for Sorter
    struct SorterDeserializeSettings {
        SorterDeserializeSettings(Version version, boost::optional<KeyFormat> ridFormat)
            : keyStringVersion(version), ridFormat(ridFormat) {}
        Version keyStringVersion;
        boost::optional<KeyFormat> ridFormat;
    };

    void serializeForSorter(BufBuilder& buf) const {
        serialize(buf);
    }

    static Value deserializeForSorter(BufReader& buf, const SorterDeserializeSettings& settings) {
        return deserialize(buf, settings.keyStringVersion, settings.ridFormat);
    }

    // It is illegal to call this function on a value that is backed by a buffer that is shared
    // elsewhere. The SharedBufferFragment cannot accurately report memory usage per individual
    // Value, so we require the sorter to look at the SharedBufferFragmentBuilder's memory usage in
    // aggregate and free unused memory periodically.
    int memUsageForSorter() const {
        invariant(!_buffer.isShared(),
                  "Cannot obtain memory usage from shared buffer on key_string::Value");
        return sizeof(Value) + _buffer.underlyingCapacity();
    }

    Value getOwned() const {
        return *this;
    }
    void makeOwned() {}

    Version getVersion() const {
        return _versionAndRidSize.version();
    }

    size_t getApproximateSize() const;

    int computeElementCount(Ordering ord) const;

private:
    static_assert(Version::kLatestVersion == Version::V1);

    VersionAndSize _versionAndRidSize;

    // _ksSize is the total length that the KeyString takes up in the buffer.
    int32_t _ksSize = 0;
    SharedBufferFragment _buffer;
};

static_assert(sizeof(Value) == 32);

/**
 * A non-owning view into a buffer populated by a completed key_string::Builder.
 *
 * This is functionally the same as Value, except it does not own the buffer and it is the user's
 * responsibility to ensure that it remains valid for as long as the view is in use.
 */
class View {
public:
    /**
     * Constructs a zero-length view pointing at a null buffer. All member functions work sensibly
     * on a default-constructed View.
     */
    constexpr View() = default;

    /**
     * Constructs a view from an explicit buffer. The buffer must contain a keystring-serialized
     * key, followed by an optional recordid, followed by the type bits. If a record id is present
     * ridSize must be non-zero.
     */
    View(Version version,
         std::span<const char> buffer,
         // The size of the RecordId part or 0 if there is no RecordId.
         uint32_t ridSize,
         // The size of the typebits part
         uint32_t tbSize) noexcept
        : _buffer(buffer.data()),
          _versionAndRidSize(version, ridSize),
          _ksSize(static_cast<uint32_t>(buffer.size() - tbSize)),
          _tbSize(tbSize) {
        // keystrings use 32-bit sizes
        invariant(buffer.size() <= std::numeric_limits<uint32_t>::max());
        // Sizes must all be zero if the buffer is a null pointer
        invariant(_buffer || (!buffer.size() && !ridSize && !tbSize));
    }

    /**
     * Constructs a View from a Value. The produced view will remain valid only as long as the
     * underlying Value exists.
     */
    /* implicit */ View(const Value& value)
        : View(value.getVersion(),
               value.getViewWithTypeBits(),
               value.getRecordIdSize(),
               value.getTypeBitsView().size()) {}

    // Returns whether the size of the stored KeyString is 0.
    bool isEmpty() const {
        return _ksSize == 0;
    }

    // Gets the span containing all three components
    std::span<const char> getCompleteView() const {
        return std::span<const char>(_buffer, _ksSize + _tbSize);
    }
    // Gets the span containing the key and record id, but not typebits
    std::span<const char> getKeyAndRecordIdView() const {
        return std::span<const char>(_buffer, _ksSize);
    }
    // Gets the span containing just the key
    std::span<const char> getKeyView() const {
        return getKeyAndRecordIdView().first(getSizeWithoutRecordId());
    }
    // Gets the span containing just the record id
    std::span<const char> getRecordIdView() const {
        return getKeyAndRecordIdView().last(_versionAndRidSize.size());
    }
    // Gets the span containing just the typebits
    std::span<const char> getTypeBitsView() const {
        return getCompleteView().subspan(_ksSize);
    }
    // Gets the span containing the record id and typebits, but not key
    std::span<const char> getRecordIdAndTypeBitsView() const {
        return getCompleteView().subspan(getSizeWithoutRecordId());
    }

    Version getVersion() const {
        return _versionAndRidSize.version();
    }

    /**
     * Returns a hex encoding of this key.
     */
    std::string toString() const;

    /**
     * Serializes this keystring, excluding the RecordId, into a storable format with TypeBits
     * information. The serialized format takes the following form:
     *     [keystring size][keystring encoding][typebits encoding]
     */
    template <typename Builder>
    void serializeWithoutRecordId(Builder& buf) const {
        auto keyView = getKeyView();
        auto tbView = getTypeBitsView();
        buf.appendNum(static_cast<int32_t>(keyView.size()));
        buf.appendBuf(keyView.data(), keyView.size());
        buf.appendBuf(tbView.data(), tbView.size());
        if (tbView.empty()) {
            buf.appendChar(0);
        }
    }

    /**
     * Deserialize a keystring from the serialized format produced by
     * serialize() or serializeWithoutRecordId(). The given ridFormat must match
     * the one used when serializing, or boost::none if
     * serializeWithoutRecordId() was used.
     *
     * The returned View points into the buffer backing the given BufReader.
     */
    static View deserialize(BufReader& buf,
                            key_string::Version version,
                            boost::optional<KeyFormat> ridFormat);

private:
    const char* _buffer = nullptr;
    VersionAndSize _versionAndRidSize;
    uint32_t _ksSize = 0;
    uint32_t _tbSize = 0;

    int32_t getSizeWithoutRecordId() const {
        return _ksSize - _versionAndRidSize.size();
    }
};
static_assert(sizeof(View) == 24);
static_assert(std::is_trivially_copyable_v<View>);

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

    /**
     * Constructs a builder given an object and ordering, stripping out top-level field names.
     * Appends the given record id to the end.
     */
    BuilderBase(Version version, const BSONObj& obj, Ordering ord, const RecordId& recordId)
        : BuilderBase(version, ord) {
        resetToKey(obj, ord, recordId);
    }

    /**
     * Constructs a builder given an object and ordering, stripping out top-level field names.
     */
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
        resetFromBuffer(other.getView());
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
        if (!_typeBits.isAllZeros()) {
            newBuf.appendBuf(_typeBits.getBuffer(), _typeBits.getSize());
        }
        // Note: this variable is needed to make sure that no method is called on 'newBuf'
        // after a call on its 'release' method.
        const size_t newBufLen = newBuf.len();
        return {
            version, _buffer().len(), _ridSize, SharedBufferFragment(newBuf.release(), newBufLen)};
    }

    /**
     * Ends the build state and returns the buffer held by the builder.
     * The buffer holds the KeyString with no RecordId or TypeBits encoded, suitable to pass into
     * SortedDataInterface::seek().
     * Caller must only use the returned buffer within the lifetime of the builder.
     *
     * If 'discriminator' is specified, then it overrides the previously set _discriminator.
     */
    std::span<const char> finishAndGetBuffer(
        boost::optional<Discriminator> discriminator = boost::none) {
        invariant(_state == BuildState::kAppendingBSONElements || _state == BuildState::kEndAdded);
        if (discriminator) {
            _discriminator = *discriminator;
        }
        _doneAppending();
        return {_buffer().buf(), static_cast<size_t>(_buffer().len())};
    }

    void appendRecordId(const RecordId& loc);
    void appendTypeBits(const TypeBits& bits);

    /**
     * Appends the given element, discarding the field name. The transformation function will be
     * applied to all string values contained in the given element.
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
     * Appends a Discriminator byte or kEnd byte to a key string.
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

    /**
     * Resets the state to the given object and ordering, stripping out top-level field names.
     * Appends the given record id to the end.
     */
    void resetToKey(const BSONObj& obj, Ordering ord, const RecordId& recordId);

    /**
     * Resets the state to the given object and ordering, stripping out top-level field names.
     */
    void resetToKey(const BSONObj& obj,
                    Ordering ord,
                    Discriminator discriminator = Discriminator::kInclusive);

    void resetFromBuffer(std::span<const char> buf) {
        _buffer().reset();
        memcpy(_buffer().skip(buf.size()), buf.data(), buf.size());
    }

    std::span<const char> getView() const {
        invariant(_state != BuildState::kReleased);
        return std::span(_buffer().buf(), _buffer().len());
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
     * Compare with another key_string::Value or Builder.
     */
    template <class T>
    int compare(const T& other) const;

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
    /**
     * Appends all elements in the given object, stripping out top-level field names.
     */
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
        if (!_typeBits.isAllZeros()) {
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

    int32_t _ridSize = 0;
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
        return {version, ksSize, _ridSize, SharedBufferFragment(_bufferBuilder.release(), bufLen)};
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
        return {version, ksSize, _ridSize, _bufferBuilder.done()};
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

// clang-format off
template <typename T, typename U>
    requires(!!isKeyString<T>())
std::strong_ordering operator<=>(const T& lhs, const U& rhs) {
    return lhs.compare(rhs) <=> 0;
}

template <typename T, typename U>
    requires(!!isKeyString<T>())
bool operator==(const T& lhs, const U& rhs) {
    return lhs.compare(rhs) == 0;
}

template <class T>
requires(!!isKeyString<T>())
std::ostream& operator<<(std::ostream& stream, const T& value) {
    return stream << value.toString();
}
// clang-format on

/**
 * Given a KeyString which may or may not have a RecordId, returns the length of the section without
 * the RecordId. More expensive than sizeWithoutRecordId(Long|Str)AtEnd
 */
size_t getKeySize(std::span<const char> buffer, Ordering ord, Version version);

/**
 * Decodes the given KeyString buffer into it's BSONObj representation. These
 * functions require that the buffer contains valid BSON and will terminate the
 * process if this is not the case.
 *
 * If the buffer provided may not be valid, use the 'Safe' version instead.
 */
BSONObj toBson(std::span<const char> data, Ordering ord, const TypeBits& types);
BSONObj toBson(std::span<const char> data,
               Ordering ord,
               std::span<const char> typeBitsRawBuffer,
               Version version);

BSONObj toBsonSafe(std::span<const char> data, Ordering ord, const TypeBits& types);
void toBsonSafe(std::span<const char> data,
                Ordering ord,
                const TypeBits& types,
                BSONObjBuilder& builder);
void toBsonSafe(std::span<const char> data,
                Ordering ord,
                TypeBits::ReaderBase& typeBitsReader,
                BSONObjBuilder& builder);

Discriminator decodeDiscriminator(std::span<const char> data,
                                  Ordering ord,
                                  const TypeBits& typeBits);

template <class T>
BSONObj toBson(const T& keyString, Ordering ord) {
    return toBson(keyString.getView(), ord, keyString.getTypeBits());
}

inline BSONObj toBson(const View& keyString, Ordering ord) {
    return toBson(keyString.getKeyAndRecordIdView(),
                  ord,
                  keyString.getTypeBitsView(),
                  keyString.getVersion());
}

/**
 * Decodes a RecordId long from the end of a buffer.
 */
RecordId decodeRecordIdLongAtEnd(std::span<const char> buf);

/**
 * Decodes a RecordId string from the end of a buffer.
 * The RecordId string length cannot be determined by looking at the start of the string.
 */
RecordId decodeRecordIdStrAtEnd(std::span<const char> buf);

/**
 * Decodes a RecordId in the given format from the end of a buffer.
 */
inline RecordId decodeRecordIdAtEnd(std::span<const char> buf, KeyFormat keyFormat) {
    return keyFormat == KeyFormat::String ? decodeRecordIdStrAtEnd(buf)
                                          : decodeRecordIdLongAtEnd(buf);
}

/**
 * Given a KeyString with a RecordId in the string format, returns the the section without the
 * RecordId.
 * If a RecordId pointer is provided, also decode the RecordId into the pointer.
 */
std::span<const char> withoutRecordIdStrAtEnd(std::span<const char> buf,
                                              RecordId* recordId = nullptr);
/**
 * Given a KeyString with a RecordId in the long format, returns the the section without the
 * RecordId.
 * If a RecordId pointer is provided, also decode the RecordId into the pointer.
 */
std::span<const char> withoutRecordIdLongAtEnd(std::span<const char> buf,
                                               RecordId* recordId = nullptr);
/**
 * Given a KeyString with a RecordId in the given format, returns the the section without the
 * RecordId.
 * If a RecordId pointer is provided, also decode the RecordId into the pointer.
 */
inline std::span<const char> withoutRecordIdAtEnd(std::span<const char> buf,
                                                  KeyFormat keyFormat,
                                                  RecordId* recordId = nullptr) {
    return keyFormat == KeyFormat::String ? withoutRecordIdStrAtEnd(buf, recordId)
                                          : withoutRecordIdLongAtEnd(buf, recordId);
}

/**
 * Given a KeyString, returns the section without the discriminator.
 */
std::span<const char> withoutDiscriminatorAtEnd(std::span<const char> buf);

/**
 * Decodes a RecordId, consuming all bytes needed from reader.
 */
RecordId decodeRecordIdLong(BufReader* reader);

/**
 * Decodes a RecordId from a buffer.
 */
RecordId decodeRecordIdLong(std::span<const char> buf);

/**
 * Perform a lexigraphical comparison of two binary buffers. This always performs an unsigned
 * comparison even if char is signed.
 */
int compare(std::span<const char> left, std::span<const char> right);

/**
 * Read one KeyString component from the given 'reader' and 'typeBits' inputs and stream it to the
 * 'BuilderInterface' object, When no components remain in the KeyString, this function returns
 * false and leaves 'stream' unmodified.
 */
bool readValue(BufReader* reader,
               TypeBits::ReaderBase* typeBits,
               bool inverted,
               Version version,
               BuilderInterface* stream);

/*
 * Appends the first field of a key string to a BSON object.
 * This does not accept TypeBits because callers of this function discard TypeBits.
 */
void appendSingleFieldToBSONAs(const char* buf,
                               int len,
                               StringData fieldName,
                               BSONObjBuilder* builder,
                               Version version = key_string::Version::kLatestVersion);

template <class BufferT>
template <class T>
int BuilderBase<BufferT>::compare(const T& other) const {
    return key_string::compare(getView(), other.getView());
}

template <class T>
int Value::compare(const T& other) const {
    return key_string::compare(getView(), other.getView());
}

template <class T>
int Value::compareWithoutRecordIdLong(const T& other) const {
    return key_string::compare(withoutRecordIdLongAtEnd(getView()),
                               withoutRecordIdLongAtEnd(other.getView()));
}

template <class T>
int Value::compareWithoutRecordIdStr(const T& other) const {
    return key_string::compare(withoutRecordIdStrAtEnd(getView()),
                               withoutRecordIdStrAtEnd(other.getView()));
}

template <class T>
int Value::compareWithoutRecordId(const T& other, KeyFormat keyFormat) const {
    return keyFormat == KeyFormat::String ? compareWithoutRecordIdStr(other)
                                          : compareWithoutRecordIdLong(other);
}

/**
 * Takes key string and key pattern information and uses it to present human-readable information
 * about an index or collection entry.
 *
 * 'logPrefix' adds a logging prefix. Useful for differentiating callers.
 */
void logKeyString(const RecordId& recordId,
                  const Value& keyStringValue,
                  const BSONObj& keyPatternBson,
                  const BSONObj& keyStringBson,
                  std::string callerLogPrefix);

BSONObj rehydrateKey(const BSONObj& keyPatternBson, const BSONObj& keyStringBson);

/**
 * Returns a human-readable output that explains each byte within the key string. For diagnostic
 * purposes only.
 *
 * If 'keyPattern' is empty or does not have as many fields as there are in the key string, fields
 * will be assumed to be ascending and will be assigned field names as empty string.
 * 'keyFormat' may be provided if the caller knows the RecordId format of this key string, if
 * any.
 */
std::string explain(std::span<const char> buffer,
                    const BSONObj& keyPattern,
                    const TypeBits& typeBits,
                    boost::optional<KeyFormat> keyFormat);

}  // namespace key_string

using KeyStringSet = boost::container::flat_set<key_string::Value>;

}  // namespace mongo
