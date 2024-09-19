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

#include <boost/container/small_vector.hpp>
#include <concepts>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/simple8b.h"
#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
namespace bsoncolumn {

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
        ContiguousBlock(ContiguousBlock&& other);
        ContiguousBlock(const ContiguousBlock&) = delete;

        ~ContiguousBlock();

        // Return pointer to contigous block and the block size
        std::pair<const char*, int> done();

    private:
        bool _active = true;
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
            return elem.type() == Object
                ? _traverseNoArrays(elem.fieldNameStringData(), elem.Obj(), Object)
                : _elemFunc(elem);
        });
    }

    bool _traverseIntoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == Object || elem.type() == Array
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
 * In the destructor, SubObjectAllocator take care of filling in the value for the object, and
 * appending the final terminating EOO.
 *
 * If the passed-in allocator is not in contiguous mode, SubObjectAllocator will start contiguous
 * mode in the constructor. In this case, it will use the Finisher to complete the object and end
 * contiguous mode in the destructor.
 */
template <typename Finisher = NoopSubObjectFinisher>
struct SubObjectAllocator {
public:
    SubObjectAllocator(ElementStorage& allocator,
                       StringData fieldName,
                       const BSONObj& obj,
                       BSONType type,
                       Finisher state = Finisher{})
        : _active(true),
          _allocator(allocator),
          _contiguousBlock(
              // If the allocator is not in contiguous mode, then start it now.
              allocator.contiguousEnabled()
                  ? boost::none
                  : boost::optional<ElementStorage::ContiguousBlock>(allocator.startContiguous())),
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
        objdata[0] = type;
        if (_fieldNameSize > 0) {
            memcpy(objdata + 1, fieldName.rawData(), _fieldNameSize);
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
    SubObjectAllocator(SubObjectAllocator&& other)
        : _active(other._active),
          _allocator(other._allocator),
          _contiguousBlock(std::move(other._contiguousBlock)),
          _finisher(std::move(other._finisher)),
          _sizeOffset(other._sizeOffset),
          _fieldNameSize(other._fieldNameSize),
          _allowEmpty(other._allowEmpty) {
        other._active = false;
    }

    SubObjectAllocator(const SubObjectAllocator&) = delete;

    ~SubObjectAllocator() {
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
    ElementStorage& _allocator;

    // ContiguousBlock RAII object for starting/stopping contiguous mode in allocator.
    boost::optional<ElementStorage::ContiguousBlock> _contiguousBlock = boost::none;

    // Finisher to invoke when exiting contiguous mode.
    Finisher _finisher;

    // Location (relative to start of contiguous mode) for size prefix of object.
    int _sizeOffset;

    // Size of the field name (not including terminating null byte)
    int _fieldNameSize;

    // Whether or not to allow creation of an empty object.
    bool _allowEmpty;
};

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
 * Helpers for block decompress-all functions
 * T - the type we are decompressing to
 * Encoding - the underlying encoding (int128_t or int64_t) for Simple8b deltas
 * Buffer - the buffer being filled by decompress()
 * Materialize - function to convert delta decoding into T and append to Buffer
 * Finish - after completion, receives the count of elements and the final element
 */

// TODO:  Materialize is used in some places to refer converting int encodings to
// concrete types, and in other places to refer to converting concrete types to
// a desired output type.  Here we use it to refer to a composite of these two
// actions; we should take the time to make our terminology consistent.

class BSONColumnBlockDecompressHelpers {
public:
    template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDelta(const char* ptr,
                                          const char* end,
                                          Buffer& buffer,
                                          Encoding last,
                                          uint64_t lastNonRLEBlock,
                                          const BSONElement& reference,
                                          const Materialize& materialize,
                                          const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                return ptr;

            uassert(8873800,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &buffer, &reference, &last](const Encoding v) {
                    if (v == 0)
                        buffer.appendLast();
                    else {
                        last = expandDelta(last, v);
                        materialize(last, reference, buffer);
                    }
                },
                [&buffer]() { buffer.appendLast(); },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDelta(const char* ptr,
                                          const char* end,
                                          Buffer& buffer,
                                          Encoding last,
                                          const BSONElement& reference,
                                          const Materialize& materialize) {
        return decompressAllDelta<T>(ptr,
                                     end,
                                     buffer,
                                     last,
                                     simple8b::kSingleZero, /* lastNonRLEBlock */
                                     reference,
                                     materialize,
                                     [](size_t count, Encoding last, uint64_t lastNonRLEBlock) {});
    }

    /* Like decompressAllDelta, but does not have branching to avoid re-materialization
       of repeated values, intended to be used on primitive types where this does not
       result in additional allocation */
    template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaPrimitive(const char* ptr,
                                                   const char* end,
                                                   Buffer& buffer,
                                                   Encoding last,
                                                   uint64_t lastNonRLEBlock,
                                                   const BSONElement& reference,
                                                   const Materialize& materialize,
                                                   const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762800,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &buffer, &reference, &last](const Encoding v) {
                    last = expandDelta(last, v);
                    materialize(last, reference, buffer);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaPrimitive(const char* ptr,
                                                   const char* end,
                                                   Buffer& buffer,
                                                   Encoding last,
                                                   const BSONElement& reference,
                                                   const Materialize& materialize) {
        return decompressAllDeltaPrimitive<T>(
            ptr,
            end,
            buffer,
            last,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            reference,
            materialize,
            [](size_t count, Encoding last, uint64_t lastNonRLEBlock) {});
    }

    template <typename T, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaOfDelta(const char* ptr,
                                                 const char* end,
                                                 Buffer& buffer,
                                                 int64_t last,
                                                 int64_t lastlast,
                                                 uint64_t lastNonRLEBlock,
                                                 const BSONElement& reference,
                                                 const Materialize& materialize,
                                                 const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762801,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<int64_t>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &lastlast, &buffer, &reference, &last](int64_t v) {
                    lastlast = expandDelta(lastlast, v);
                    last = expandDelta(last, lastlast);
                    materialize(last, reference, buffer);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastlast, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaOfDelta(const char* ptr,
                                                 const char* end,
                                                 Buffer& buffer,
                                                 int64_t last,
                                                 const BSONElement& reference,
                                                 const Materialize& materialize) {
        return decompressAllDeltaOfDelta<T>(
            ptr,
            end,
            buffer,
            last,
            0,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            reference,
            materialize,
            [](size_t count, int64_t last, int64_t lastlast, uint64_t lastNonRLEBlock) {});
    }

    template <class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDouble(const char* ptr,
                                           const char* end,
                                           Buffer& buffer,
                                           double last,
                                           uint64_t lastNonRLEBlock,
                                           const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        int64_t lastValue = 0;
        size_t elemCount = 0;
        uint8_t scaleIndex = bsoncolumn::kInvalidScaleIndex;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            scaleIndex = bsoncolumn::scaleIndexForControlByte(control);
            uassert(8762802,
                    "Invalid control byte in BSON Column",
                    scaleIndex != bsoncolumn::kInvalidScaleIndex);
            auto encodedDouble = Simple8bTypeUtil::encodeDouble(last, scaleIndex);
            uassert(8295701, "Invalid double encoding in BSON Column", encodedDouble);
            lastValue = *encodedDouble;

            elemCount += simple8b::visitAll<int64_t>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&last, &buffer, &scaleIndex, &lastValue](int64_t v) {
                    lastValue = expandDelta(lastValue, v);
                    last = Simple8bTypeUtil::decodeDouble(lastValue, scaleIndex);
                    buffer.append(last);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, lastValue, scaleIndex, lastNonRLEBlock);
        return ptr;
    }

    template <class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllMissing(const char* ptr,
                                            const char* end,
                                            Buffer& buffer,
                                            uint64_t lastNonRLEBlock,
                                            const Finish& finish) {
        size_t elemCount = 0;
        while (ptr < end) {
            const uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                break;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8915000,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount = simple8b::count(ptr + 1, size);
            for (size_t i = 0; i < elemCount; ++i) {
                buffer.appendMissing();
            }

            ptr += 1 + size;
        }

        finish(elemCount, lastNonRLEBlock);
        return ptr;
    }

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllMissing(const char* ptr, const char* end, Buffer& buffer) {
        return decompressAllMissing(ptr,
                                    end,
                                    buffer,
                                    simple8b::kSingleZero /* lastNonRLEBlock */,
                                    [](size_t count, uint64_t lastNonRLEBlock) {});
    }

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllDouble(const char* ptr,
                                           const char* end,
                                           Buffer& buffer,
                                           double last) {
        return decompressAllDouble(
            ptr,
            end,
            buffer,
            last,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            [](size_t count, int64_t last, uint8_t scaleIndex, uint64_t lastNonRLEBlock) {});
    }

    template <typename Encoding, class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllLiteral(const char* ptr,
                                            const char* end,
                                            Buffer& buffer,
                                            uint64_t lastNonRLEBlock,
                                            const Finish& finish) {
        if (buffer.isLastMissing()) {
            // The last element in the buffer is missing (EOO).
            return decompressAllMissing(ptr, end, buffer, lastNonRLEBlock, finish);
        }

        size_t elemCount = 0;
        while (ptr < end) {
            const uint8_t control = *ptr;
            if (control == EOO || isUncompressedLiteralControlByte(control) ||
                isInterleavedStartControlByte(control))
                break;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762803,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&buffer](const Encoding v) {
                    uassert(
                        8609800, "Post literal delta blocks should only contain skip or 0", v == 0);
                    buffer.appendLast();
                },
                [&buffer]() { buffer.appendLast(); },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, lastNonRLEBlock);
        return ptr;
    }

    template <typename Encoding, class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllLiteral(const char* ptr, const char* end, Buffer& buffer) {
        return decompressAllLiteral<Encoding>(ptr,
                                              end,
                                              buffer,
                                              simple8b::kSingleZero /* lastNonRLEBlock */,
                                              [](size_t count, uint64_t lastNonRLEBlock) {});
    }

    static bool containsScalars(const BSONObj& obj) {
        bool result = false;
        BSONObjTraversal{true,
                         BSONType::Object,
                         [](auto&&...) { return true; },
                         [&](auto&&...) {
                             result = true;
                             return false;
                         }}
            .traverse(obj);

        return result;
    }
};

/**
 * Implements the "materializer" concept such that the output elements are BSONElements.
 */
class BSONElementMaterializer {
public:
    using Element = BSONElement;

    static BSONElement materialize(ElementStorage& allocator, bool val);
    static BSONElement materialize(ElementStorage& allocator, int32_t val);
    static BSONElement materialize(ElementStorage& allocator, int64_t val);
    static BSONElement materialize(ElementStorage& allocator, double val);
    static BSONElement materialize(ElementStorage& allocator, const Decimal128& val);
    static BSONElement materialize(ElementStorage& allocator, const Date_t& val);
    static BSONElement materialize(ElementStorage& allocator, const Timestamp& val);
    static BSONElement materialize(ElementStorage& allocator, StringData val);
    static BSONElement materialize(ElementStorage& allocator, const BSONBinData& val);
    static BSONElement materialize(ElementStorage& allocator, const BSONCode& val);
    static BSONElement materialize(ElementStorage& allocator, const OID& val);

    template <typename T>
    static BSONElement materialize(ElementStorage& allocator, BSONElement val) {
        auto allocatedElem = allocator.allocate(val.type(), "", val.valuesize());
        memcpy(allocatedElem.value(), val.value(), val.valuesize());
        return allocatedElem.element();
    }

    static BSONElement materializePreallocated(BSONElement val) {
        return val;
    }

    static BSONElement materializeMissing(ElementStorage& allocator) {
        return BSONElement();
    }

    static bool isMissing(const Element& elem) {
        return elem.eoo();
    }

private:
    /**
     * Helper function used by both BSONCode and String.
     */
    static BSONElement writeStringData(ElementStorage& allocator,
                                       BSONType bsonType,
                                       StringData val);
};

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<bool>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Bool, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.boolean());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<int32_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberInt, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int32_t)val._numberInt());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<int64_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberLong, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int64_t)val._numberLong());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<double>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberDouble, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val._numberDouble());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Decimal128>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberDecimal, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val._numberDecimal());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Date_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Date, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.date());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Timestamp>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == bsonTimestamp, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.timestamp());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<StringData>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == String, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.valueStringData());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<BSONBinData>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BinData, "materialize invoked with incorrect BSONElement type");
    int len = 0;
    const char* data = val.binData(len);
    return materialize(allocator, BSONBinData(data, len, val.binDataType()));
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<BSONCode>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Code, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, BSONCode(val.valueStringData()));
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<OID>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == jstOID, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.OID());
}

struct RootPath {
    boost::container::small_vector<const char*, 1> elementsToMaterialize(BSONObj refObj) {
        return {refObj.objdata()};
    }
};

/**
 * Returns true if the given path is the root path. If it returns anything given the empty object,
 * then it's the root path.
 */
template <class Path>
bool isRootPath(Path& path) {
    return !path.elementsToMaterialize(BSONObj{}).empty();
}

}  // namespace bsoncolumn
}  // namespace mongo
