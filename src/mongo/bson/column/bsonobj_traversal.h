// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bson_element_storage.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/container/small_vector.hpp>

namespace mongo::bsoncolumn {
using namespace std::literals::string_view_literals;
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
            return _traverseIntoArrays(""sv, obj, _rootType);
        } else {
            return _traverseNoArrays(""sv, obj, _rootType);
        }
    }

private:
    bool _traverseNoArrays(std::string_view fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == BSONType::object
                ? _traverseNoArrays(elem.fieldNameStringData(), elem.Obj(), BSONType::object)
                : _elemFunc(elem);
        });
    }

    bool _traverseIntoArrays(std::string_view fieldName, const BSONObj& obj, BSONType type) {
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
class BSONSubObjectAllocator {
public:
    BSONSubObjectAllocator(BSONElementStorage& allocator,
                           std::string_view fieldName,
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
}  // namespace mongo::bsoncolumn
