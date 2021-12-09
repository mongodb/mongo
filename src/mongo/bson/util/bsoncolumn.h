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

namespace mongo {

/**
 * The BSONColumn class represents a reference to a BSONElement of BinDataType 7, which can
 * efficiently store any BSONArray and also allows for missing values. At a high level, two
 * optimizations are applied:
 *   - implied field names: do not store decimal keys representing index keys.
 *   - delta compression using Simple-8b: store difference between subsequent scalars of the same
 * type
 *
 * The BSONColumn will not take ownership of the BinData element, but otherwise implements
 * an interface similar to BSONObj. Because iterators over the BSONColumn need to rematerialize
 * deltas, they use additional storage owned by the BSONColumn for this. As all iterators will
 * create new delta's in the same order, they share a single ElementStore, with a worst-case memory
 * usage bounded to a total size on the order of that of the size of the expanded BSONColumn.
 *
 * All iterators are invalidated when moving the BSONColumn.
 */
class BSONColumn {
public:
    BSONColumn(BSONElement bin);
    BSONColumn(BSONBinData bin, StringData name);

    /**
     * Forward iterator type to access BSONElement from BSONColumn.
     *
     * Default-constructed BSONElement (EOO type) represent missing value.
     * Returned BSONElement are owned by BSONColumn instance and should not be kept after the
     * BSONColumn instance goes out of scope.
     */
    class Iterator {
    public:
        friend class BSONColumn;

        // typedefs expected in iterators
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = BSONElement;
        using pointer = const BSONElement*;
        using reference = const BSONElement&;

        reference operator*() const {
            return _column->_decompressed.at(_index);
        }
        pointer operator->() const {
            return &operator*();
        }

        // pre-increment operator
        Iterator& operator++();

        // post-increment operator
        Iterator operator++(int);

        bool operator==(const Iterator& rhs) const;
        bool operator!=(const Iterator& rhs) const;

        // Move this Iterator to a new BSONColumn instance. Should only be used when moving
        // BSONColumn instances and we want to re-attach the iterator to the new instance without
        // losing position
        Iterator moveTo(BSONColumn& column);

    private:
        Iterator(BSONColumn& column, const char* pos, const char* end);

        // Initializes Iterator and makes it ready for iteration. Provided index must be 0 or point
        // to a full literal.
        void _initialize(size_t index);

        // Initialize sub-object interleaving from current control byte position. Must be on a
        // interleaved start byte.
        void _initializeInterleaving();

        // Helpers to increment the iterator in regular and interleaved mode.
        void _incrementRegular();
        void _incrementInterleaved();

        // Handles EOO when in regular mode. Iterator is set to end.
        void _handleEOO();

        // Checks if control byte is literal
        static bool _isLiteral(uint8_t control) {
            return (control & 0xE0) == 0;
        }

        // Checks if control byte is interleaved mode start
        static bool _isInterleavedStart(uint8_t control) {
            return control == 0xF0;
        }

        // Returns number of Simple-8b blocks from control byte
        static uint8_t _numSimple8bBlocks(uint8_t control) {
            return (control & 0x0F) + 1;
        }

        // Pointer to BSONColumn this Iterator is created from, this will be stale when moving the
        // BSONColumn. All iterators are invalidated on move!
        BSONColumn* _column;

        // Current iterator position
        size_t _index = 0;

        // Current control byte on iterator position
        const char* _control;

        // End of BSONColumn memory block, we may not dereference any memory passed this.
        const char* _end;

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
                bool full;
            };

            // Loads a literal
            void _loadLiteral(const BSONElement& elem);

            // Loads current control byte
            LoadControlResult _loadControl(BSONColumn& column,
                                           const char* buffer,
                                           const char* end,
                                           const BSONElement* current);

            // Loads delta value
            BSONElement _loadDelta(BSONColumn& column,
                                   const boost::optional<uint64_t>& delta,
                                   const BSONElement* current);
            BSONElement _loadDelta(BSONColumn& column,
                                   const boost::optional<uint128_t>& delta,
                                   const BSONElement* current);

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
    };

    /**
     * Forward iterator access.
     *
     * Iterator value is EOO when element is skipped.
     *
     * Iterators materialize compressed BSONElement as they iterate over the compressed binary.
     * It is NOT safe to do this from multiple threads concurrently.
     *
     * Throws if invalid encoding is encountered.
     */
    Iterator begin();
    Iterator end();

    /**
     * Element lookup by index
     *
     * Returns EOO if index represent skipped element.
     * Returns boost::none if index is out of bounds.
     *
     * O(1) time complexity if element has been previously accessed
     * O(N) time complexity otherwise
     *
     * Materializes compressed BSONElement as needed. It is NOT safe to do this from multiple
     * threads concurrently.
     *
     * Throws if invalid encoding is encountered.
     */
    boost::optional<const BSONElement&> operator[](size_t index);

    /**
     * Number of elements stored in this BSONColumn
     *
     * O(1) time complexity if BSONColumn is fully decompressed (iteration reached end).
     * O(N) time complexity otherwise, will fully decompress BSONColumn.
     *
     * * Throws if invalid encoding is encountered.
     */
    size_t size();

    /**
     * Field name that this BSONColumn represents.
     *
     * O(1) time complexity
     */
    StringData name() const {
        return _name;
    }

private:
    /**
     * BSONElement storage, owns materialised BSONElement returned by BSONColumn.
     * Allocates memory in blocks which double in size as they grow.
     */
    class ElementStorage {
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
     * Validates the BSONColumn on init(). Should be the last call in the constructor when all
     * members are initialized.
     */
    void _init();

    struct SubObjectAllocator;

    std::deque<BSONElement> _decompressed;
    ElementStorage _elementStorage;

    const char* _binary;
    int _size;

    struct DecodingStartPosition {
        void setIfLarger(size_t index, const char* control);

        const char* _control = nullptr;
        size_t _index = 0;
    };
    DecodingStartPosition _maxDecodingStartPos;

    bool _fullyDecompressed = false;

    std::string _name;
};
}  // namespace mongo
