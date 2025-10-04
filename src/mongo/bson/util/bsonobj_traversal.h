/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"

#include <boost/container/small_vector.hpp>

namespace mongo {
/**
 * BSONElement storage, owns materialised BSONElement returned by BSONColumn.
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

/**
 * Helper class to perform recursion over a BSONObj. Two functions are provided:
 *
 * EnterSubObjFunc is called before recursing deeper, it may output an RAII object to perform logic
 * when entering/exiting a subobject.
 *
 * ElementFunc is called for every non-object element encountered during the recursion.
 */
template <typename EnterSubObjFunc, typename ElementFunc>
class BSONObjTraversal {
public:
    BSONObjTraversal(bool recurseIntoArrays,
                     BSONType rootType,
                     EnterSubObjFunc enterFunc,
                     ElementFunc elemFunc)
        : _enterFunc(std::move(enterFunc)),
          _elemFunc(std::move(elemFunc)),
          _recurseIntoArrays(recurseIntoArrays),
          _rootType(rootType) {}

    bool traverse(const BSONObj& obj) {
        if (_recurseIntoArrays) {
            return _traverseIntoArrays(""_sd, obj, _rootType);
        } else {
            return _traverseNoArrays(""_sd, obj, _rootType);
        }
    }

private:
    bool _traverseNoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == BSONType::object
                ? _traverseNoArrays(elem.fieldNameStringData(), elem.Obj(), BSONType::object)
                : _elemFunc(elem);
        });
    }

    bool _traverseIntoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == BSONType::object || elem.type() == BSONType::array
                ? _traverseIntoArrays(elem.fieldNameStringData(), elem.Obj(), elem.type())
                : _elemFunc(elem);
        });
    }

    EnterSubObjFunc _enterFunc;
    ElementFunc _elemFunc;
    bool _recurseIntoArrays;
    BSONType _rootType;
};

struct NoopSubObjectFinisher {
    void finish(const char* elemBytes, int fieldNameSize) {}
    void finishMissing() {}
};

/**
 * Helper RAII class that assists with materializing BSONElements that contain objects or arrays.
 * Its constructor will take care of writing the header bytes of an object to the allocator,
 * including the field name and space for the length of the object.
 *
 * During this object's lifetime it's expected that the fields will be written to the allocator.
 *
 * In the destructor, BSONSubObjectAllocator take care of filling in the value for the object, and
 * appending the final terminating EOO.
 *
 * If the passed-in allocator is not in contiguous mode, BSONSubObjectAllocator will start
 * contiguous mode in the constructor. In this case, it will use the Finisher to complete the object
 * and end contiguous mode in the destructor.
 */
template <typename Finisher = NoopSubObjectFinisher>
struct BSONSubObjectAllocator {
public:
    BSONSubObjectAllocator(BSONElementStorage& allocator,
                           StringData fieldName,
                           const BSONObj& obj,
                           BSONType type,
                           Finisher state = Finisher{})
        : _active(true),
          _allocator(allocator),
          _contiguousBlock(
              // If the allocator is not in contiguous mode, then start it now.
              allocator.contiguousEnabled() ? boost::none
                                            : boost::optional<BSONElementStorage::ContiguousBlock>(
                                                  allocator.startContiguous())),
          _finisher(std::move(state)) {
        invariant(_allocator.contiguousEnabled());

        // Remember size of field name for this subobject in case it ends up being an empty
        // subobject and we need to 'deallocate' it.
        _fieldNameSize = fieldName.size();
        // We can allow an empty subobject if it existed in the reference object
        _allowEmpty = obj.isEmpty();

        // Start the subobject, allocate space for the field in the parent which is BSON type byte +
        // field name + null terminator
        char* objdata = _allocator.allocate(2 + _fieldNameSize);
        objdata[0] = stdx::to_underlying(type);
        if (_fieldNameSize > 0) {
            memcpy(objdata + 1, fieldName.data(), _fieldNameSize);
        }
        objdata[_fieldNameSize + 1] = '\0';

        // BSON Object type begins with a 4 byte count of number of bytes in the object. Reserve
        // space for this count and remember the offset so we can set it later when the size is
        // known. Storing offset over pointer is needed in case we reallocate to a new memory block.
        _sizeOffset = _allocator.position() - _allocator.contiguous();
        _allocator.allocate(4);
    }

    /**
     * Move constructor. Make sure that only the destination remains active to enforce RAII
     * semantics.
     */
    BSONSubObjectAllocator(BSONSubObjectAllocator&& other)
        : _active(other._active),
          _allocator(other._allocator),
          _contiguousBlock(std::move(other._contiguousBlock)),
          _finisher(std::move(other._finisher)),
          _sizeOffset(other._sizeOffset),
          _fieldNameSize(other._fieldNameSize),
          _allowEmpty(other._allowEmpty) {
        other._active = false;
    }

    BSONSubObjectAllocator(const BSONSubObjectAllocator&) = delete;

    ~BSONSubObjectAllocator() {
        if (_active) {
            invariant(_allocator.contiguousEnabled());
            // Check if we wrote no subfields in which case we are an empty subobject that needs to
            // be omitted
            if (!_allowEmpty &&
                _allocator.position() == _allocator.contiguous() + _sizeOffset + 4) {
                _allocator.deallocate(_fieldNameSize + 6);
                if (_contiguousBlock) {
                    _finisher.finishMissing();
                }
                return;
            }

            // Write the EOO byte to end the object and fill out the first 4 bytes for the size that
            // we reserved in the constructor.
            auto eoo = _allocator.allocate(1);
            *eoo = '\0';
            int32_t size = _allocator.position() - _allocator.contiguous() - _sizeOffset;
            DataView(_allocator.contiguous() + _sizeOffset).write<LittleEndian<uint32_t>>(size);

            if (_contiguousBlock) {
                // If we started contiguous mode upon construction, finish the object.
                auto [ptr, size] = _contiguousBlock->done();
                _finisher.finish(ptr, _fieldNameSize + 1);
            }
        }
    }


private:
    // Whether or not this object is active. Will be false if this was moved.
    bool _active;

    // Allocator to which to write the subobject.
    BSONElementStorage& _allocator;

    // ContiguousBlock RAII object for starting/stopping contiguous mode in allocator.
    boost::optional<BSONElementStorage::ContiguousBlock> _contiguousBlock = boost::none;

    // Finisher to invoke when exiting contiguous mode.
    Finisher _finisher;

    // Location (relative to start of contiguous mode) for size prefix of object.
    int _sizeOffset;

    // Size of the field name (not including terminating null byte)
    int _fieldNameSize;

    // Whether or not to allow creation of an empty object.
    bool _allowEmpty;
};
}  // namespace mongo
