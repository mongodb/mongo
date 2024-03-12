/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <absl/numeric/int128.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bsoncolumn_helpers.h"
#include "mongo/bson/util/bsoncolumn_interleaved.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/simple8b.h"
#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/platform/int128.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
using namespace bsoncolumn;

/**
 * The BSONColumn class represents an implementation to interpret a BSONElement of BinDataType 7,
 * which can efficiently store any BSONArray in a compact representation. The format has the
 * following high-level features and capabilities:
 *   - implied field names: decimal keys representing index keys are not stored.
 *   - type specific delta/delta-of-delta compression stored using Simple-8b: difference between
 *     subsequent scalars of the same type are stored with as few bits as possible.
 *   - doubles are scaled and rounded to nearest integer for efficient storage.
 *   - internal encoding for missing values.
 *   - run-length-encoding for efficient storage of large number of repeated values
 *   - object/array compression where scalars are internally stored as separate interleaved
 *     BSONColumn compressed binary streams.
 *
 * The BSONColumn will not take ownership of the provided binary, but otherwise implements an
 * interface similar to BSONObj.
 *
 * Iterators over the BSONColumn need to materialize BSONElement from deltas and use additional
 * storage owned by the BSONColumn. All BSONElements returned remain valid while the BSONColumn is
 * kept in scope. Multiple passes grows memory usage which is not free'd until the BSONColumn goes
 * out of scope or the release() function is called.
 *
 * Thread safety: The BSONColumn class is generally NOT thread-safe, unless declared otherwise. This
 * also applies to functions declared 'const'.
 */
class BSONColumn {
public:
    BSONColumn(const char* buffer, size_t size);
    explicit BSONColumn(BSONElement bin);
    explicit BSONColumn(BSONBinData bin);

    /**
     * Input iterator type to access BSONElement from BSONColumn.
     *
     * A default-constructed BSONElement (EOO type) represents a missing value. Returned
     * BSONElements are owned by the BSONColumn instance and should not be kept after the BSONColumn
     * instance goes out of scope.
     *
     * Iterator can be used either as an STL iterator with begin() and end() or as a non-STL
     * iterator via begin() and incrementing until more() returns false.
     */
    class Iterator {
    public:
        friend class BSONColumn;

        // Constructs a begin iterator
        Iterator(boost::intrusive_ptr<ElementStorage> allocator, const char* pos, const char* end);

        // typedefs expected in iterators
        using iterator_category = std::input_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = BSONElement;
        using pointer = const BSONElement*;
        using reference = const BSONElement&;

        // Constructs an end iterator
        Iterator() = default;

        reference operator*() const {
            return _decompressed;
        }
        pointer operator->() const {
            return &operator*();
        }

        // pre-increment operator
        Iterator& operator++();

        bool operator==(const Iterator& rhs) const {
            return _index == rhs._index;
        }
        bool operator!=(const Iterator& rhs) const {
            return !operator==(rhs);
        }

        /**
         * Returns true if iterator may be incremented. Equivalent to comparing not equal with the
         * end iterator.
         */
        bool more() const {
            return _control != _end;
        }

    private:
        friend class BSONColumnBuilder;

        // Initialize sub-object interleaving from current control byte position. Must be on a
        // interleaved start byte.
        void _initializeInterleaving();

        // Handles EOO when in regular mode. Iterator is set to end.
        void _handleEOO();

        // Sentinel to represent end iterator
        static constexpr uint32_t kEndIndex = 0xFFFFFFFF;

        // Current iterator value
        BSONElement _decompressed;

        // Current iterator position
        uint32_t _index = kEndIndex;

        // Current control byte on iterator position
        const char* _control = nullptr;

        // End of BSONColumn memory block, we may not dereference any memory past this.
        const char* _end = nullptr;

        // ElementStorage to use when materializing elements
        boost::intrusive_ptr<ElementStorage> _allocator;

        /**
         * Decoding state for decoding compressed binary into BSONElement. It is detached from the
         * actual binary to allow interleaving where control bytes corresponds to separate decoding
         * states.
         */
        struct DecodingState {
            DecodingState();

            /**
             * Internal decoding state for types using 64bit aritmetic
             */
            struct Decoder64 {
                Decoder64();

                BSONElement materialize(ElementStorage& allocator,
                                        BSONElement last,
                                        StringData fieldName) const;

                Simple8b<uint64_t>::Iterator pos;
                int64_t lastEncodedValue = 0;
                int64_t lastEncodedValueForDeltaOfDelta = 0;
                uint8_t scaleIndex;
                bool deltaOfDelta = false;
            };

            /**
             * Internal decoding state for types using 128bit aritmetic
             */
            struct Decoder128 {
                BSONElement materialize(ElementStorage& allocator,
                                        BSONElement last,
                                        StringData fieldName) const;


                Simple8b<uint128_t>::Iterator pos;
                int128_t lastEncodedValue = 0;
            };

            struct LoadControlResult {
                BSONElement element;
                int size;
            };

            // Loads a literal
            void loadUncompressed(const BSONElement& elem);

            // Loads current control byte
            LoadControlResult loadControl(ElementStorage& allocator,
                                          const char* buffer,
                                          const char* end);

            // Loads delta value
            BSONElement loadDelta(ElementStorage& allocator, Decoder64& decoder);
            BSONElement loadDelta(ElementStorage& allocator, Decoder128& decoder);

            // Last encoded values used to calculate delta and delta-of-delta
            BSONElement lastValue;
            std::variant<Decoder64, Decoder128> decoder = Decoder64{};
        };

        /**
         * Internal state for regular decoding mode (decoding of scalars)
         */
        struct Regular {
            DecodingState state;
        };

        /**
         * Internal state for interleaved decoding mode (decoding of objects/arrays)
         */
        struct Interleaved {
            Interleaved(BSONObj refObj, BSONType referenceObjType, bool interleavedArrays);

            std::vector<DecodingState> states;

            // Interleaving reference object read when encountered the interleaving start control
            // byte. We setup a decoding state for each scalar field in this object. The object
            // hierarchy is used to re-construct with full objects with the correct hierachy to the
            // user.
            BSONObj referenceObj;

            // Indicates if decoding states should be opened when encountering arrays
            bool arrays;

            // Type for root object/reference object. May be Object or Array.
            BSONType rootType;
        };

        // Helpers to increment the iterator in regular and interleaved mode.
        void _incrementRegular(Regular& regular);
        void _incrementInterleaved(Interleaved& interleaved);

        std::variant<Regular, Interleaved> _mode = Regular{};
    };

    /**
     * Input iterator access.
     *
     * Iterator value is EOO when element is skipped.
     *
     * Iterators materialize compressed BSONElement as they iterate over the compressed binary.
     * Grows memory usage for this BSONColumn.
     *
     * It is NOT safe to call this or iterate from multiple threads concurrently.
     *
     * Throws if invalid encoding is encountered.
     */
    Iterator begin() const;
    Iterator end() const;

    /**
     * Element lookup by index
     *
     * Returns EOO if index represent skipped element.
     * Returns boost::none if index is out of bounds.
     *
     * O(N) time complexity
     *
     * Materializes BSONElement as needed and grows memory usage for this BSONColumn.
     *
     * It is NOT safe to call this from multiple threads concurrently.
     *
     * Throws if invalid encoding is encountered.
     */
    boost::optional<BSONElement> operator[](size_t index) const;

    /**
     * Number of elements stored in this BSONColumn
     *
     * O(N) time complexity
     *
     * Materializes BSONElements internally and grows memory usage for this BSONColumn.

     * It is NOT safe to call this from multiple threads concurrently.
     *
     * Throws if invalid encoding is encountered.
     */
    size_t size() const;

    // Scans the compressed BSON Column format to efficiently determine if the
    // column contains an element of type `elementType`.
    // Because it is marked const, it always iterates over the entire column.
    //
    // TODO SERVER-74926: add interleaved support
    bool contains_forTest(BSONType elementType) const;

    /**
     * Releases memory that has been used to materialize BSONElements for this BSONColumn.
     *
     * The returned reference counted pointer holds are reference to the previously materialized
     * BSONElements and can be used to extend their lifetime over the BSONColumn.
     *
     * It is NOT safe to call this from multiple threads concurrently.
     */
    boost::intrusive_ptr<ElementStorage> release();

private:
    /**
     * Validates the BSONColumn on construction, should be the last call in the constructor when all
     * members are initialized.
     */
    void _initialValidate();

    const char* _binary;
    int _size;

    // Reference counted allocator, used to allocate memory when materializing BSONElements.
    boost::intrusive_ptr<ElementStorage> _allocator;
};

// Avoid GCC/Clang compiler issues
// See
// https://stackoverflow.com/questions/53408962/try-to-understand-compiler-error-message-default-member-initializer-required-be
inline BSONColumn::Iterator::DecodingState::DecodingState() = default;
inline BSONColumn::Iterator::DecodingState::Decoder64::Decoder64() = default;

namespace bsoncolumn {

/**
 * Code below is work in progress, do not use.
 */

/**
 * Interface for a buffer to receive decoded elements from block-based
 * BSONColumn decompression.
 */
template <class T>
concept Appendable = requires(
    T& t, StringData strVal, BSONBinData binVal, BSONCode codeVal, BSONElement bsonVal, int32_t n) {
    t.append(true);
    t.append((int32_t)1);
    t.append((int64_t)1);
    t.append(Decimal128());
    t.append((double)1.0);
    t.append((Timestamp)1);
    t.append(Date_t::now());
    t.append(OID::gen());
    t.append(strVal);
    t.append(binVal);
    t.append(codeVal);

    // Strings can arrive either in 128-bit encoded format, or as
    // literals (BSONElement)

    // Takes pre-allocated BSONElement
    t.template append<bool>(bsonVal);
    t.template append<int32_t>(bsonVal);
    t.template append<int64_t>(bsonVal);
    t.template append<Decimal128>(bsonVal);
    t.template append<double>(bsonVal);
    t.template append<Timestamp>(bsonVal);
    t.template append<Date_t>(bsonVal);
    t.template append<OID>(bsonVal);
    t.template append<StringData>(bsonVal);
    t.template append<BSONBinData>(bsonVal);
    t.template append<BSONCode>(bsonVal);
    t.template append<BSONElement>(bsonVal);

    t.appendPreallocated(bsonVal);

    t.appendPositionInfo(n);

    t.appendMissing();

    // Repeat the last appended value
    t.appendLast();
};

/**
 * Interface to accept elements decoded from BSONColumn and materialize them
 * as Elements of user-defined type.
 *
 * This class will be used with decompress() and other methods of BSONColumn to efficiently produce
 * values of the desired type (e.g., SBE values or BSONElements). The methods provided by
 * implementors of this concept will be called from the main decompression loop, so they should be
 * inlineable, and avoid branching and memory allocations when possible.
 *
 * The data types passed to the materialize() methods could be referencing memory on the stack
 * (e.g., the pointer in a StringData instance) and so implementors should assume this data is
 * ephemeral. The provided ElementStorage can be used to allocate memory with the lifetime of the
 * BSONColumn instance.
 *
 * The exception to this rule is that BSONElements passed to the materialize() methods may appear in
 * decompressed form as-is in the BSONColumn binary data. If they are as such, they will have the
 * same lifetime as the BSONColumn, and may go away if a yield of query execution occurs.
 * Implementers may wish to explicitly copy the value with the allocator in this case. It may also
 * occur that decompression allocates its own BSONElements as part of its execution (e.g., when
 * materializing whole objects from compressed scalars). In this case, decompression will invoke
 * materializePreallocated() instead of materialize().
 */
template <class T>
concept Materializer = requires(T& t,
                                ElementStorage& alloc,
                                StringData strVal,
                                BSONBinData binVal,
                                BSONCode codeVal,
                                BSONElement bsonVal) {
    { T::materialize(alloc, true) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (int32_t)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (int64_t)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, Decimal128()) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (double)1.0) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (Timestamp)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, Date_t::now()) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, OID::gen()) } -> std::same_as<typename T::Element>;

    { T::materialize(alloc, strVal) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, binVal) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, codeVal) } -> std::same_as<typename T::Element>;

    { T::template materialize<bool>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<int32_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<int64_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Decimal128>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<double>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Timestamp>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Date_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<OID>(alloc, bsonVal) } -> std::same_as<typename T::Element>;

    { T::template materialize<StringData>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<BSONBinData>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<BSONCode>(alloc, bsonVal) } -> std::same_as<typename T::Element>;

    { T::materializePreallocated(bsonVal) } -> std::same_as<typename T::Element>;

    { T::materializeMissing(alloc) } -> std::same_as<typename T::Element>;
};

/**
 * Interface to indicate to the 'Collector' at compile time if the user requested the decompressor
 * to collect the position information of values within documents.
 */
template <typename T>
concept PositionInfoAppender = requires(T& t, int32_t n) {
    { t.appendPositionInfo(n) } -> std::same_as<void>;
};

/**
 * Implements Appendable and utilizes a user-defined Materializer to receive output of
 * BSONColumn decoding and fill a container of user-defined elements.  Container can
 * be user-defined or any STL container can be used.
 */
template <class CMaterializer, class Container>
requires Materializer<CMaterializer>
class Collector {
    using Element = typename CMaterializer::Element;

public:
    Collector(Container& collection, boost::intrusive_ptr<ElementStorage> allocator)
        : _collection(collection), _allocator(std::move(allocator)) {}

    static constexpr bool kCollectsPositionInfo = PositionInfoAppender<Container>;

    void append(bool val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(int32_t val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(int64_t val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(Decimal128 val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(double val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(Timestamp val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(Date_t val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(OID val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(const StringData& val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(const BSONBinData& val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    void append(const BSONCode& val) {
        _collection.push_back(CMaterializer::materialize(*_allocator, val));
    }

    template <typename T>
    void append(const BSONElement& val) {
        _collection.push_back(CMaterializer::template materialize<T>(*_allocator, val));
    }

    void appendPreallocated(const BSONElement& val) {
        _collection.push_back(CMaterializer::materializePreallocated(val));
    }

    void appendMissing() {
        _collection.push_back(CMaterializer::materializeMissing(*_allocator));
    }

    void appendLast() {
        _collection.push_back(_collection.back());
    }

    void appendPositionInfo(int32_t n) {
        // If the 'Container' doesn't request position information, this will be a no-op.
        if constexpr (kCollectsPositionInfo) {
            _collection.appendPositionInfo(n);
        }
    }

private:
    Container& _collection;
    boost::intrusive_ptr<ElementStorage> _allocator;
};

class BSONColumnBlockBased {

public:
    BSONColumnBlockBased(const char* buffer, size_t size);
    explicit BSONColumnBlockBased(BSONBinData bin);

    /**
     * Decompress entire BSONColumn
     *
     */
    template <class Buffer>
    requires Appendable<Buffer>
    void decompress(Buffer& buffer) const;

    /**
     * Wrapper that expects the caller to define a Materializer and
     * a Container to receive a collection of elements from block decoding
     */
    template <class CMaterializer, class Container>
    requires Materializer<CMaterializer>
    void decompress(Container& collection, boost::intrusive_ptr<ElementStorage> allocator) const {
        Collector<CMaterializer, Container> collector(collection, allocator);
        decompress(collector);
    }

    /**
     * Version of decompress that accepts multiple paths decompressed to separate buffers.
     */
    template <class CMaterializer, class Container, typename Path>
    requires Materializer<CMaterializer>
    void decompress(boost::intrusive_ptr<ElementStorage> allocator,
                    std::span<std::pair<Path, Container>> paths) const;

    /*
     * Decompress entire BSONColumn using the iteration-based implementation. This is used for
     * testing and production uses should eventually be replaced.
     */
    template <class Buffer>
    requires Appendable<Buffer>
    void decompressIterative(Buffer& buffer, boost::intrusive_ptr<ElementStorage> allocator) const {
        BSONColumn::Iterator it(allocator, _binary, _binary + _size);
        for (; it.more(); ++it) {
            buffer.appendPreallocated(*it);
        }
    }

    /**
     * Wrapper that expects the caller to define a Materializer and a Container to receive a
     * collection of elements from block decoding. This calls the iteration-based implementation.
     * This is used for testing and production uses should eventually be replaced.
     */
    template <class CMaterializer, class Container>
    requires Materializer<CMaterializer>
    void decompressIterative(Container& collection,
                             boost::intrusive_ptr<ElementStorage> allocator) const {
        Collector<CMaterializer, Container> collector(collection, allocator);
        decompressIterative(collector, std::move(allocator));
    }

    /**
     * Return first non-missing element stored in this BSONColumn
     */
    BSONElement first() const;

    /**
     * Return last non-missing element stored in this BSONColumn
     */
    BSONElement last() const;

    /**
     * Return 'min' element in this BSONColumn.
     *
     * TODO: Do we need to specify ComparisonRulesSet here?
     */
    BSONElement min(const StringDataComparator* comparator = nullptr) const;

    /**
     * Return 'max' element in this BSONColumn.
     *
     * TODO: Do we need to specify ComparisonRulesSet here?
     */
    BSONElement max(const StringDataComparator* comparator = nullptr) const;

    /**
     * Return sum of all elements stored in this BSONColumn.
     *
     * The BSONColumn must only contain NumberInt, NumberLong, NumberDouble, NumberDecimal types,
     * throws otherwise.
     */
    BSONElement sum() const;

    /**
     * Element lookup by index
     *
     * Returns EOO if index represent skipped element.
     * Returns boost::none if index is out of bounds.
     */
    boost::optional<BSONElement> operator[](size_t index) const;

    /**
     * Number of elements stored (including 'missing') in this BSONColumn
     */
    size_t size() const;

    /**
     * Returns true if 'type' is stored within the BSONColumn. Traverses any internal objects if
     * 'type' is a scalar.
     */
    bool contains(BSONType type) const;

private:
    const char* _binary;
    size_t _size;

    /**
     * Helpers for block decompress-all functions
     * T - the type we are decompressing to
     * Encoding - the underlying encoding (int128_t or int64_t) for Simple8b deltas
     * Buffer - the buffer being filled by decompress()
     * Materialize - function to convert delta decoding into T and append to Buffer
     * Decode - the Simple8b decoder to use
     */

    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDelta(const char* ptr,
                                          const char* end,
                                          Buffer& buffer,
                                          Encoding last,
                                          const BSONElement& reference,
                                          const Materialize& materialize);

    /* Like decompressAllDelta, but does not have branching to avoid re-materialization
       of repeated values, intended to be used on primitive types where this does not
       result in additional allocation */
    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaPrimitive(const char* ptr,
                                                   const char* end,
                                                   Buffer& buffer,
                                                   Encoding last,
                                                   const BSONElement& reference,
                                                   const Materialize& materialize);

    template <typename T, class Buffer, typename Materialize, typename Decode>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaOfDelta(const char* ptr,
                                                 const char* end,
                                                 Buffer& buffer,
                                                 int64_t last,
                                                 const BSONElement& reference,
                                                 const Materialize& materialize,
                                                 const Decode& decode);

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllDouble(const char* ptr,
                                           const char* end,
                                           Buffer& buffer,
                                           double reference);

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllLiteral(const char* ptr,
                                            const char* end,
                                            Buffer& buffer,
                                            const BSONElement& reference);
};

/**
 * Version of decompress() that accepts multiple paths decompressed to separate buffers.
 */
template <class CMaterializer, class Container, typename Path>
requires Materializer<CMaterializer>
void BSONColumnBlockBased::decompress(boost::intrusive_ptr<ElementStorage> allocator,
                                      std::span<std::pair<Path, Container>> paths) const {
    std::vector<std::pair<Path, Collector<CMaterializer, Container>>> pathCollectors;
    for (auto&& p : paths) {
        pathCollectors.push_back(
            {p.first, Collector<CMaterializer, Container>{p.second, allocator}});
    }

    const char* control = _binary;
    const char* end = _binary + _size;
    while (*control != EOO) {
        BlockBasedInterleavedDecompressor decompressor{*allocator, control, end};
        invariant(bsoncolumn::isInterleavedStartControlByte(*control),
                  "non-interleaved data is not yet handled via this API");
        control = decompressor.decompress(pathCollectors);
        invariant(control < end);
    }
}

}  // namespace bsoncolumn
}  // namespace mongo

#include "bsoncolumn.inl"
