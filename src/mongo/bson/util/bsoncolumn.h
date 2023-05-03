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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/simple8b.h"

#include <deque>
#include <memory>
#include <vector>

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
private:
    class ElementStorage;

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
     */
    class Iterator {
    public:
        friend class BSONColumn;

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

    private:
        // Constructs a begin iterator
        Iterator(boost::intrusive_ptr<ElementStorage> allocator, const char* pos, const char* end);

        // Initialize sub-object interleaving from current control byte position. Must be on a
        // interleaved start byte.
        void _initializeInterleaving();

        // Helpers to increment the iterator in regular and interleaved mode.
        void _incrementRegular();
        void _incrementInterleaved();

        // Handles EOO when in regular mode. Iterator is set to end.
        void _handleEOO();

        // Checks if control byte is literal
        static bool _isLiteral(char control);

        // Checks if control byte is interleaved mode start
        static bool _isInterleavedStart(char control);

        // Returns number of Simple-8b blocks from control byte
        static uint8_t _numSimple8bBlocks(char control);

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

        // Allocator to use when materializing elements
        boost::intrusive_ptr<ElementStorage> _allocator;

        // Helper to create Simple8b decoding iterators for 64bit and 128bit value types.
        // previousValue is used in case the first Simple8b block is RLE and this value will then be
        // used for the RLE repeat.
        template <typename T>
        struct Decoder {
            Decoder(const char* buf, size_t size, const boost::optional<T>& previousValue)
                : s8b(buf, size, previousValue), pos(s8b.begin()), end(s8b.end()) {}

            Simple8b<T> s8b;
            typename Simple8b<T>::Iterator pos;
            typename Simple8b<T>::Iterator end;
        };

        /**
         * Decoding state for decoding compressed binary into BSONElement. It is detached from the
         * actual binary to allow interleaving where control bytes corresponds to separate decoding
         * states.
         */
        struct DecodingState {
            struct LoadControlResult {
                BSONElement element;
                int size;
            };

            // Loads a literal
            void _loadLiteral(const BSONElement& elem);

            // Loads current control byte
            LoadControlResult _loadControl(ElementStorage& allocator,
                                           const char* buffer,
                                           const char* end);

            // Loads delta value
            BSONElement _loadDelta(ElementStorage& allocator,
                                   const boost::optional<uint64_t>& delta);
            BSONElement _loadDelta(ElementStorage& allocator,
                                   const boost::optional<uint128_t>& delta);

            // Decoders, only one should be instantiated at a time.
            boost::optional<Decoder<uint64_t>> _decoder64;
            boost::optional<Decoder<uint128_t>> _decoder128;

            // Last encoded values used to calculate delta and delta-of-delta
            BSONType _lastType;
            bool _deltaOfDelta;
            BSONElement _lastValue;
            int64_t _lastEncodedValue64 = 0;
            int64_t _lastEncodedValueForDeltaOfDelta = 0;
            int128_t _lastEncodedValue128 = 0;

            // Current scale index
            uint8_t _scaleIndex;
        };

        // Decoding states. Interleaved mode is active when '_states' is not empty. When in regular
        // mode we use '_state'.
        DecodingState _state;
        std::vector<DecodingState> _states;

        // Interleaving reference object read when encountered the interleaving start control byte.
        // We setup a decoding state for each scalar field in this object. The object hierarchy is
        // used to re-construct with full objects with the correct hierachy to the user.
        BSONObj _interleavedReferenceObj;

        // Indicates if decoding states should be opened when encountering arrays
        bool _interleavedArrays;

        // Type for root object/reference object. May be Object or Array.
        BSONType _interleavedRootType;
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
     * BSONElement storage, owns materialised BSONElement returned by BSONColumn.
     * Allocates memory in blocks which double in size as they grow.
     */
    class ElementStorage
        : public boost::intrusive_ref_counter<ElementStorage, boost::thread_unsafe_counter> {
    public:
        /**
         * "Writable" BSONElement. Provides access to a writable pointer for writing the value of
         * the BSONElement. Users must write valid BSON data depending on the requested BSON type.
         */
        class Element {
        public:
            Element(char* buffer, int nameSize, int valueSize);

            /**
             * Returns a pointer for writing a BSONElement value.
             */
            char* value();

            /**
             * Size for the pointer returned by value()
             */
            int size() const;

            /**
             * Constructs a BSONElement from the owned buffer.
             */
            BSONElement element() const;

        private:
            char* _buffer;
            int _nameSize;
            int _valueSize;
        };

        /**
         * RAII Helper to manage contiguous mode. Starts on construction and leaves on destruction.
         */
        class ContiguousBlock {
        public:
            ContiguousBlock(ElementStorage& storage);
            ~ContiguousBlock();

            const char* done();

        private:
            ElementStorage& _storage;
            bool _finished = false;
        };

        /**
         * Allocates provided number of bytes. Returns buffer that is safe to write up to that
         * amount. Any subsequent call to allocate() or deallocate() invalidates the returned
         * buffer.
         */
        char* allocate(int bytes);

        /**
         * Allocates a BSONElement of provided type and value size. Field name is set to empty
         * string.
         */
        Element allocate(BSONType type, StringData fieldName, int valueSize);

        /**
         * Deallocates provided number of bytes. Moves back the pointer of used memory so it can be
         * re-used by the next allocate() call.
         */
        void deallocate(int bytes);

        /**
         * Starts contiguous mode. All allocations will be in a contiguous memory block. When
         * allocate() need to grow contents from previous memory block is copied.
         */
        ContiguousBlock startContiguous();

        /**
         * Returns writable pointer to the beginning of contiguous memory block. Any call to
         * allocate() will invalidate this pointer.
         */
        char* contiguous() const {
            return _block.get() + _contiguousPos;
        }

        /**
         * Returns pointer to the end of current memory block. Any call to allocate() will
         * invalidate this pointer.
         */
        const char* position() const {
            return _block.get() + _pos;
        }

    private:
        // Starts contiguous mode
        void _beginContiguous();

        // Ends contiguous mode
        void _endContiguous();

        // Full memory blocks that are kept alive.
        std::vector<std::unique_ptr<char[]>> _blocks;

        // Current memory block
        std::unique_ptr<char[]> _block;

        // Capacity of current memory block
        int _capacity = 0;

        // Position to first unused byte in current memory block
        int _pos = 0;

        // Position to beginning of contiguous block if enabled.
        int _contiguousPos = 0;

        bool _contiguousEnabled = false;
    };

    /**
     * Validates the BSONColumn on construction, should be the last call in the constructor when all
     * members are initialized.
     */
    void _initialValidate();

    struct SubObjectAllocator;

    const char* _binary;
    int _size;

    // Reference counted allocator, used to allocate memory when materializing BSONElements.
    boost::intrusive_ptr<ElementStorage> _allocator;
};
}  // namespace mongo
