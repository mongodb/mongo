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
#include "mongo/bson/util/simple8b.h"

#include <deque>
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
 */
class BSONColumn {
public:
    BSONColumn(BSONElement bin);

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
            return _column._decompressed.at(_index);
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

    private:
        Iterator(BSONColumn& column, const char* pos, const char* end);

        // Loads current control byte
        void _loadControl(const BSONElement& prev);

        // Loads delta value
        void _loadDelta(const BSONElement& prev, const boost::optional<uint64_t>& delta);
        void _loadDelta(const BSONElement& prev, const boost::optional<uint128_t>& delta);

        // Helpers to determine if we need to store uncompressed element when advancing iterator
        bool _needStoreElement() const;
        void _storeElementIfNeeded(BSONElement elem);

        // Checks if control byte is literal
        static bool _literal(uint8_t control) {
            return (control & 0xE0) == 0;
        }

        // Returns number of Simple-8b blocks from control byte
        static uint8_t _numSimple8bBlocks(uint8_t control) {
            return (control & 0x0F) + 1;
        }

        // Reference to BSONColumn this Iterator is created from
        BSONColumn& _column;

        // Current iterator position
        size_t _index = 0;

        // Last index observed with a non-skipped value
        size_t _lastValueIndex = 0;

        // Last encoded values used to calculate delta and delta-of-delta
        int64_t _lastEncodedValue64 = 0;
        int64_t _lastEncodedValueForDeltaOfDelta = 0;
        int128_t _lastEncodedValue128 = 0;

        // Current control byte on iterator position
        const char* _control;

        // End of BSONColumn memory block, we may not dereference any memory passed this.
        const char* _end;

        // Helper to create Simple8b decoding iterators for 64bit and 128bit value types.
        template <typename T>
        struct Decoder {
            Decoder(const char* buf, size_t size)
                : s8b(buf, size), pos(s8b.begin()), end(s8b.end()) {}

            Simple8b<T> s8b;
            typename Simple8b<T>::Iterator pos;
            typename Simple8b<T>::Iterator end;
        };

        // Decoders, only one should be instantiated at a time.
        boost::optional<Decoder<uint64_t>> _decoder64;
        boost::optional<Decoder<uint128_t>> _decoder128;

        // Current scale index
        uint8_t _scaleIndex;
    };

    /**
     * Forward iterator access.
     *
     * Iterator value is EOO
     *
     * Iterators materialize compressed BSONElement as they iterate over the compressed binary.
     * It is NOT safe to do this from multiple threads concurrently.
     *
     * Throws BadValue if invalid encoding is encountered.
     */
    Iterator begin();
    Iterator end();

    /**
     * Element lookup by index
     *
     * Returns EOO if index represent skipped element or is out of bounds.
     * O(1) time complexity if element has been previously accessed
     * O(N) time complexity otherwise
     *
     * Materializes compressed BSONElement as needed. It is NOT safe to do this from multiple
     * threads concurrently.
     *
     * Throws BadValue if invalid encoding is encountered.
     */
    BSONElement operator[](size_t index);

    /**
     * Number of elements stored in this BSONColumn
     *
     * O(1) time complexity if BSONColumn is fully decompressed
     * O(N) time complexity otherwise
     *
     * Throws BadValue if invalid encoding is encountered
     */
    size_t size() const;

private:
    /**
     * BSONElement storage, owns materialised BSONElement returned by BSONColumn.
     * Allocates memory in blocks which double in size as they grow.
     */
    class ElementStorage {
    public:
        class Element {
        public:
            Element(char* buffer, int valueSize);

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
            int _valueSize;
        };

        /**
         * Allocates a BSONElement of provided type and value size. Field name is set to empty
         * string.
         */
        Element allocate(BSONType type, int valueSize);

    private:
        // Full memory blocks that are kept alive.
        std::vector<std::unique_ptr<char[]>> _blocks;

        // Current memory block
        std::unique_ptr<char[]> _block;

        // Capacity of current memory block
        int _capacity = 0;

        // Position to first unused byte in current memory block
        int _pos = 0;
    };

    std::deque<BSONElement> _decompressed;
    ElementStorage _elementStorage;

    const char* _binary;
    int _size;

    const char* _controlLastLiteral;
    size_t _indexLastLiteral = 0;
    bool _fullyDecompressed = false;
};
}  // namespace mongo
