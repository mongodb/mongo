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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/column/bsoncolumn_helpers.h"
#include "mongo/bson/column/bsoncolumn_interleaved.h"
#include "mongo/bson/column/bsoncolumn_util.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bsonobj_traversal.h"
#include "mongo/platform/int128.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <variant>
#include <vector>

#include <absl/numeric/int128.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

namespace mongo {

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
        Iterator(boost::intrusive_ptr<BSONElementStorage> allocator,
                 const char* pos,
                 const char* end);

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
        template <class Allocator>
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

        // BSONElementStorage to use when materializing elements
        boost::intrusive_ptr<BSONElementStorage> _allocator;

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

                BSONElement materialize(BSONElementStorage& allocator,
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
                BSONElement materialize(BSONElementStorage& allocator,
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
            LoadControlResult loadControl(BSONElementStorage& allocator,
                                          const char* buffer,
                                          const char* end);

            // Loads delta value
            BSONElement loadDelta(BSONElementStorage& allocator, Decoder64& decoder);
            BSONElement loadDelta(BSONElementStorage& allocator, Decoder128& decoder);

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
    boost::intrusive_ptr<BSONElementStorage> release();

private:
    /**
     * Validates the BSONColumn on construction, should be the last call in the constructor when all
     * members are initialized.
     */
    void _initialValidate();

    const char* _binary;
    int _size;

    // Reference counted allocator, used to allocate memory when materializing BSONElements.
    boost::intrusive_ptr<BSONElementStorage> _allocator;
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
 * Implements Appendable and utilizes a user-defined Materializer to receive output of
 * BSONColumn decoding and fill a container of user-defined elements.  Container can
 * be user-defined or any STL container can be used.
 */
template <class CMaterializer, class Container>
requires Materializer<CMaterializer>
class Collector {
    using Element = typename CMaterializer::Element;

public:
    Collector(Container& collection, boost::intrusive_ptr<BSONElementStorage> allocator)
        : _collection(collection), _allocator(std::move(allocator)) {}

    static constexpr bool kCollectsPositionInfo = PositionInfoAppender<Container>;

    void eof() {}

    void append(bool val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(int32_t val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(int64_t val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(Decimal128 val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(double val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(Timestamp val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(Date_t val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(OID val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(StringData val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(const BSONBinData& val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    void append(const BSONCode& val) {
        _last = CMaterializer::materialize(*_allocator, val);
        _collection.push_back(_last);
    }

    template <typename T>
    void append(const BSONElement& val) {
        _last = CMaterializer::template materialize<T>(*_allocator, val);
        _collection.push_back(_last);
    }

    void appendPreallocated(const BSONElement& val) {
        _last = CMaterializer::materializePreallocated(val);
        _collection.push_back(_last);
    }

    // Does not update _last, should not be repeated by appendLast()
    void appendMissing() {
        _collection.push_back(CMaterializer::materializeMissing(*_allocator));
    }

    // Appends last value that was not Missing
    void appendLast() {
        _collection.push_back(_last);
    }

    bool isLastMissing() {
        return CMaterializer::isMissing(_last);
    }

    // Sets the last value without appending anything. This should be called to update _last to be
    // the element in the reference object, and for missing top-level objects. Otherwise the append
    // methods will take care of updating _last as needed.
    template <typename T>
    void setLast(const BSONElement& val) {
        _last = CMaterializer::template materialize<T>(*_allocator, val);
    }

    void appendPositionInfo(int32_t n) {
        // If the 'Container' doesn't request position information, this will be a no-op.
        if constexpr (kCollectsPositionInfo) {
            _collection.appendPositionInfo(n);
        }
    }

    BSONElementStorage& getAllocator() {
        return *_allocator;
    }

private:
    Container& _collection;
    boost::intrusive_ptr<BSONElementStorage> _allocator;
    Element _last = CMaterializer::materializeMissing(*_allocator);
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
    inline void decompress(Buffer& buffer) const;

    /**
     * Wrapper that expects the caller to define a Materializer and
     * a Container to receive a collection of elements from block decoding
     */
    template <class CMaterializer, class Container>
    requires Materializer<CMaterializer>
    void decompress(Container& collection,
                    boost::intrusive_ptr<BSONElementStorage> allocator) const {
        Collector<CMaterializer, Container> collector(collection, allocator);
        decompress(collector);
    }

    /**
     * Version of decompress that accepts multiple paths decompressed to separate buffers.
     */
    template <class CMaterializer, class Container, typename Path>
    requires Materializer<CMaterializer>
    void decompress(boost::intrusive_ptr<BSONElementStorage> allocator,
                    std::span<std::pair<Path, Container&>> paths) const;

    /*
     * Decompress entire BSONColumn using the iteration-based implementation. This is used for
     * testing and production uses should eventually be replaced.
     */
    template <class Buffer>
    requires Appendable<Buffer>
    void decompressIterative(Buffer& buffer,
                             boost::intrusive_ptr<BSONElementStorage> allocator) const {
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
                             boost::intrusive_ptr<BSONElementStorage> allocator) const {
        Collector<CMaterializer, Container> collector(collection, allocator);
        decompressIterative(collector, std::move(allocator));
    }

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
};

/**
 * Version of decompress() that accepts multiple paths decompressed to separate buffers.
 *
 * This function cannot yet handle arbitrary BSONColumn data:
 * - Elements in the BSONColumn must be either objects or missing (EOO).
 * - Uncompressed literals may appear in the BSONColumn only if they are empty objects.
 * - Any other object data must be encoded in interleaved mode.
 */
template <class CMaterializer, class Container, typename Path>
requires Materializer<CMaterializer>
void BSONColumnBlockBased::decompress(boost::intrusive_ptr<BSONElementStorage> allocator,
                                      std::span<std::pair<Path, Container&>> paths) const {

    // The Collector class wraps a reference to a buffer passed in by the caller.
    // BlockBasedInterleavedDecompressor expects references to collectors, so create a vector where
    // we allocate them.
    std::vector<Collector<CMaterializer, Container>> ownedCollectors;
    // Create a vector of pairs of paths and references to collectors, to be passed to the
    // decompressor.
    std::vector<std::pair<Path, Collector<CMaterializer, Container>&>> pathCollectors;
    ownedCollectors.reserve(paths.size());
    pathCollectors.reserve(paths.size());
    for (auto&& p : paths) {
        ownedCollectors.emplace_back(p.second, allocator);
        pathCollectors.push_back({p.first, ownedCollectors.back()});
    }

    const char* ptr = _binary;
    const char* end = _binary + _size;
    const uint64_t lastNonRLEBlock = simple8b::kSingleZero;

    // If there are any leading simple8b blocks, they are all skips. Handle them first.
    uint8_t control = *ptr;
    if (isSimple8bControlByte(control)) {
        const char* newPtr = nullptr;
        for (auto&& collector : ownedCollectors) {
            newPtr = BSONColumnBlockDecompressHelpers::decompressAllMissing(
                ptr, end, collector, lastNonRLEBlock, [&collector](size_t count, uint64_t) {
                    for (size_t i = 0; i < count; ++i) {
                        collector.appendPositionInfo(1);
                    }
                });
        }
        ptr = newPtr;
    }

    while (ptr < end) {
        control = *ptr;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            uassert(
                8517800, "BSONColumn data ended without reaching end of buffer", ptr + 1 == end);
            break;
        } else if (isUncompressedLiteralControlByte(control)) {
            // The BSONColumn encoding guarantees that the field name is just a single null byte.
            BSONElement literal(ptr, 1, BSONElement::TrustedInitTag{});
            dassert(BSONElement{ptr}.fieldNameSize() == 1,  // size includes null byte
                    "Unexpected field name in top-level BSONColumn element");
            BSONType type = literal.type();
            ptr += literal.size();
            switch (type) {
                case BSONType::object: {
                    const auto obj = literal.Obj();
                    // TODO(SERVER-88217) Remove when we can evaluate paths in arbitrary objects.
                    invariant(obj.isEmpty() ||
                                  !BSONColumnBlockDecompressHelpers::containsScalars(obj),
                              "object literal with scalars in path-based decompress()");
                    for (auto&& pathPair : pathCollectors) {
                        if (isRootPath(pathPair.first)) {
                            pathPair.second.template append<BSONObj>(literal);
                        } else {
                            pathPair.second.appendMissing();
                            pathPair.second.template setLast<BSONElement>(BSONElement{});
                        }
                        pathPair.second.appendPositionInfo(1);
                    }
                    break;
                }
                default:
                    // TODO SERVER-88215 Remove this assertion once we Handle other data types in
                    // here.
                    invariant(false, "unhandled data type in path-based decompress()");
            }

            if (isSimple8bControlByte(*ptr)) {
                const char* newPtr = nullptr;
                for (auto&& collector : ownedCollectors) {
                    newPtr = BSONColumnBlockDecompressHelpers::decompressAllLiteral<int64_t>(
                        ptr, end, collector, lastNonRLEBlock, [&collector](size_t count, uint64_t) {
                            for (size_t i = 0; i < count; ++i) {
                                collector.appendPositionInfo(1);
                            }
                        });
                }
                ptr = newPtr;
            }
        } else if (isInterleavedStartControlByte(control)) {
            BlockBasedInterleavedDecompressor decompressor{*allocator, ptr, end};
            ptr = decompressor.decompress(std::span{pathCollectors});

            // If there are any simple8b blocks after the interleaved section, handle them now.
            if (isSimple8bControlByte(*ptr)) {
                const char* newPtr = nullptr;
                for (auto&& collector : ownedCollectors) {
                    newPtr = BSONColumnBlockDecompressHelpers::decompressAllLiteral<int64_t>(
                        ptr, end, collector, lastNonRLEBlock, [&collector](size_t count, uint64_t) {
                            for (size_t i = 0; i < count; ++i) {
                                collector.appendPositionInfo(1);
                            }
                        });
                }
                ptr = newPtr;
            }
        } else {
            uasserted(8517801, "Unexpected control byte value");
        }
    }
}

}  // namespace bsoncolumn
}  // namespace mongo

#include "bsoncolumn.inl"
