/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

namespace MONGO_MOD_PUBLIC mongo {
/**
 * BSONElement storage, owns materialized BSONElement returned by BSONColumn.
 * Allocates memory in blocks which double in size as they grow.
 */
class BSONElementStorage
    : public boost::intrusive_ref_counter<BSONElementStorage, boost::thread_unsafe_counter> {
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
        ContiguousBlock(BSONElementStorage& storage);
        ContiguousBlock(ContiguousBlock&& other);
        ContiguousBlock(const ContiguousBlock&) = delete;

        ~ContiguousBlock();

        // Return pointer to contigous block and the block size
        std::pair<const char*, int> done();

    private:
        bool _active = true;
        BSONElementStorage& _storage;
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

    bool contiguousEnabled() const {
        return _contiguousEnabled;
    }

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

    // Ends contiguous mode, returns size of block
    int _endContiguous();

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
}  // namespace MONGO_MOD_PUBLIC mongo
