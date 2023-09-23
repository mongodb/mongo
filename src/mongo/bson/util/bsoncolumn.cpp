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

#include "mongo/bson/util/bsoncolumn.h"

#include <algorithm>
#include <array>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <cstdint>
#include <cstring>
#include <utility>

#include <absl/numeric/int128.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

namespace mongo {
using namespace bsoncolumn;

namespace {
// Start capacity for memory blocks allocated by ElementStorage
constexpr int kStartCapacity = 128;

// Max capacity for memory blocks allocated by ElementStorage. We need to allow blocks to grow to at
// least BSONObjMaxUserSize so we can construct user objects efficiently.
constexpr int kMaxCapacity = BSONObjMaxUserSize;

// Memory offset to get to BSONElement value when field name is an empty string.
constexpr int kElementValueOffset = 2;

// Lookup table to go from Control byte (high 4 bits) to scale index.
constexpr uint8_t kInvalidScaleIndex = 0xFF;
constexpr std::array<uint8_t, 16> kControlToScaleIndex = {
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    kInvalidScaleIndex,
    Simple8bTypeUtil::kMemoryAsInteger,  // 0b1000
    0,                                   // 0b1001
    1,                                   // 0b1010
    2,                                   // 0b1011
    3,                                   // 0b1100
    4,                                   // 0b1101
    kInvalidScaleIndex,
    kInvalidScaleIndex};


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

}  // namespace

BSONColumn::ElementStorage::Element::Element(char* buffer, int nameSize, int valueSize)
    : _buffer(buffer), _nameSize(nameSize), _valueSize(valueSize) {}

char* BSONColumn::ElementStorage::Element::value() {
    // Skip over type byte and null terminator for field name
    return _buffer + _nameSize + kElementValueOffset;
}

int BSONColumn::ElementStorage::Element::size() const {
    return _valueSize;
}

BSONElement BSONColumn::ElementStorage::Element::element() const {
    return {_buffer,
            _nameSize + 1,
            _valueSize + _nameSize + kElementValueOffset,
            BSONElement::TrustedInitTag{}};
}

BSONColumn::ElementStorage::ContiguousBlock::ContiguousBlock(ElementStorage& storage)
    : _storage(storage) {
    _storage._beginContiguous();
}

BSONColumn::ElementStorage::ContiguousBlock::~ContiguousBlock() {
    if (!_finished) {
        _storage._endContiguous();
    }
}

std::pair<const char*, int> BSONColumn::ElementStorage::ContiguousBlock::done() {
    auto ptr = _storage.contiguous();
    int size = _storage._endContiguous();
    _finished = true;
    return std::make_pair(ptr, size);
}

char* BSONColumn::ElementStorage::allocate(int bytes) {
    // If current block doesn't have enough capacity we need to allocate a new one.
    if (_capacity - _pos < bytes) {
        // Keep track of current block if it exists.
        if (_block) {
            _blocks.push_back(std::move(_block));
        }

        // If contiguous mode is enabled we need to copy data from the previous block
        auto bytesFromPrevBlock = 0;
        if (_contiguousEnabled) {
            bytesFromPrevBlock = _pos - _contiguousPos;
        }

        // Double block size while keeping it within [kStartCapacity, kMaxCapacity] range, unless a
        // size larger than kMaxCapacity is requested.
        _capacity = std::max(std::clamp(_capacity * 2, kStartCapacity, kMaxCapacity),
                             bytes + bytesFromPrevBlock);
        _block = std::make_unique<char[]>(_capacity);

        // Copy data from the previous block if contiguous mode is enabled.
        if (bytesFromPrevBlock > 0) {
            memcpy(_block.get(), _blocks.back().get() + _contiguousPos, bytesFromPrevBlock);
        }
        _contiguousPos = 0;
        _pos = bytesFromPrevBlock;
    }

    // Increment the used size and return
    auto pos = _pos;
    _pos += bytes;
    return _block.get() + pos;
}

void BSONColumn::ElementStorage::deallocate(int bytes) {
    _pos -= bytes;
}

BSONColumn::ElementStorage::ContiguousBlock BSONColumn::ElementStorage::startContiguous() {
    return ContiguousBlock(*this);
}

void BSONColumn::ElementStorage::_beginContiguous() {
    _contiguousPos = _pos;
    _contiguousEnabled = true;
}

int BSONColumn::ElementStorage::_endContiguous() {
    _contiguousEnabled = false;
    return _pos - _contiguousPos;
}

BSONColumn::ElementStorage::Element BSONColumn::ElementStorage::allocate(BSONType type,
                                                                         StringData fieldName,
                                                                         int valueSize) {
    // Size needed for this BSONElement
    auto fieldNameSize = fieldName.size();
    int size = valueSize + fieldNameSize + kElementValueOffset;

    auto block = allocate(size);

    // Write type and null terminator in the first two bytes
    block[0] = type;
    if (fieldNameSize != 0) {
        memcpy(block + 1, fieldName.rawData(), fieldNameSize);
    }
    block[fieldNameSize + 1] = '\0';

    // Construct the Element, current block will have enough size at this point
    return Element(block, fieldNameSize, valueSize);
}

struct BSONColumn::SubObjectAllocator {
public:
    SubObjectAllocator(ElementStorage& allocator,
                       StringData fieldName,
                       const BSONObj& obj,
                       BSONType type)
        : _allocator(allocator) {
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

    ~SubObjectAllocator() {
        // Check if we wrote no subfields in which case we are an empty subobject that needs to be
        // omitted
        if (!_allowEmpty && _allocator.position() == _allocator.contiguous() + _sizeOffset + 4) {
            _allocator.deallocate(_fieldNameSize + 6);
            return;
        }

        // Write the EOO byte to end the object and fill out the first 4 bytes for the size that we
        // reserved in the constructor.
        auto eoo = _allocator.allocate(1);
        *eoo = '\0';
        int32_t size = _allocator.position() - _allocator.contiguous() - _sizeOffset;
        DataView(_allocator.contiguous() + _sizeOffset).write<LittleEndian<uint32_t>>(size);
    }

private:
    ElementStorage& _allocator;
    int _sizeOffset;
    int _fieldNameSize;
    bool _allowEmpty;
};

BSONColumn::Iterator::Iterator(boost::intrusive_ptr<ElementStorage> allocator,
                               const char* pos,
                               const char* end)
    : _index(0), _control(pos), _end(end), _allocator(std::move(allocator)), _mode(Regular{}) {
    // Initialize the iterator state to the first element
    _incrementRegular(stdx::get<Regular>(_mode));
}

void BSONColumn::Iterator::_initializeInterleaving() {
    Interleaved& interleaved = _mode.emplace<Interleaved>(
        BSONObj(_control + 1),
        *_control == bsoncolumn::kInterleavedStartArrayRootControlByte ? Array : Object,
        *_control == bsoncolumn::kInterleavedStartControlByte ||
            *_control == bsoncolumn::kInterleavedStartArrayRootControlByte);

    BSONObjTraversal t(
        interleaved.arrays,
        interleaved.rootType,
        [](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
        [&interleaved](const BSONElement& elem) {
            interleaved.states.emplace_back();
            interleaved.states.back().loadUncompressed(elem);
            return true;
        });
    t.traverse(interleaved.referenceObj);
    uassert(6067610, "Invalid BSONColumn encoding", !interleaved.states.empty());

    _control += interleaved.referenceObj.objsize() + 1;
    _incrementInterleaved(interleaved);
}

BSONColumn::Iterator& BSONColumn::Iterator::operator++() {
    ++_index;

    stdx::visit(OverloadedVisitor{[&](Regular& regular) { _incrementRegular(regular); },
                                  [&](Interleaved& interleaved) {
                                      _incrementInterleaved(interleaved);
                                  }},
                _mode);

    return *this;
}

void BSONColumn::Iterator::_incrementRegular(Regular& regular) {
    DecodingState& state = regular.state;

    if (auto d64 = stdx::get_if<DecodingState::Decoder64>(&state.decoder)) {
        // Traverse current Simple8b block for 64bit values if it exists
        if (d64->pos.valid() && (++d64->pos).more()) {
            _decompressed = state.loadDelta(*_allocator, *d64);
            return;
        }
    } else if (auto d128 = stdx::get_if<DecodingState::Decoder128>(&state.decoder)) {
        // Traverse current Simple8b block for 128bit values if it exists
        if (d128->pos.valid() && (++d128->pos).more()) {
            _decompressed = state.loadDelta(*_allocator, *d128);
            return;
        }
    }

    // We don't have any more delta values in current block so we need to load next control byte.
    // Validate that we are not reading out of bounds
    uassert(6067602, "Invalid BSON Column encoding", _control < _end);

    // Decoders are exhausted, load next control byte. If we are at EOO then decoding is done.
    if (*_control == EOO) {
        _handleEOO();
        return;
    }

    // Load new control byte
    if (_isInterleavedStart(*_control)) {
        _initializeInterleaving();
        return;
    }
    auto result = state.loadControl(*_allocator, _control, _end);
    _decompressed = result.element;
    _control += result.size;
}
void BSONColumn::Iterator::_incrementInterleaved(Interleaved& interleaved) {
    // Notify the internal allocator to keep all allocations in contigous memory. That way we can
    // produce the full BSONObj that we need to return.
    auto contiguous = _allocator->startContiguous();

    // Iterate over the reference interleaved object. We match scalar subfields with our interleaved
    // states in order. Internally the necessary recursion is performed and the second lambda below
    // is called for scalar fields. Every element writes its data to the allocator so a full BSONObj
    // is produced, this usually happens within _loadDelta() but must explicitly be done in the
    // cases where re-materialization of the Element wasn't required (same as previous for example).
    // The first lambda outputs an RAII object that is instantiated every time we recurse deeper.
    // This handles writing the BSONObj size and EOO bytes for subobjects.
    auto stateIt = interleaved.states.begin();
    auto stateEnd = interleaved.states.end();
    int processed = 0;
    BSONObjTraversal t(
        interleaved.arrays,
        interleaved.rootType,
        [this](StringData fieldName, const BSONObj& obj, BSONType type) {
            // Called every time we recurse into a subobject. It makes sure we write the size and
            // EOO bytes.
            return SubObjectAllocator(*_allocator, fieldName, obj, type);
        },
        [this, &stateIt, &stateEnd, &processed](const BSONElement& referenceField) {
            // Called for every scalar field in the reference interleaved BSONObj. We have as many
            // decoding states as scalars.
            uassert(6067603, "Invalid BSON Column interleaved encoding", stateIt != stateEnd);
            auto& state = *(stateIt++);

            // Remember the iterator position before writing anything. This is to detect that
            // nothing was written and we need to copy the element into the allocator position.
            auto allocatorPosition = _allocator->position();
            BSONElement elem;
            // Load deltas if decoders are setup. nullptr is always used for "current". So even if
            // we are iterating the second time we are going to allocate new memory. This is a
            // tradeoff to avoid a decoded list of literals for every state that will only be used
            // if we iterate multiple times.
            if (auto d64 = stdx::get_if<DecodingState::Decoder64>(&state.decoder);
                d64 && d64->pos.valid() && (++d64->pos).more()) {
                elem = state.loadDelta(*_allocator, *d64);
            } else if (auto d128 = stdx::get_if<DecodingState::Decoder128>(&state.decoder);
                       d128 && d128->pos.valid() && (++d128->pos).more()) {
                elem = state.loadDelta(*_allocator, *d128);
            } else if (*_control == EOO) {
                // Decoders are exhausted and the next control byte was EOO then we should exit
                // interleaved mode. Return false to end the recursion early.
                ++_control;
                return false;
            } else {
                // Decoders are exhausted so we need to load the next control byte that by
                // definition belong to this decoder state as we iterate in the same known order.
                auto result = state.loadControl(*_allocator, _control, _end);
                _control += result.size;
                elem = result.element;

                // If the loaded control byte was a literal it is stored without field name. We need
                // to create a new BSONElement with the field name added as this is a sub-field in
                // an object.
                auto fieldName = referenceField.fieldNameStringData();
                if (!elem.eoo() && elem.fieldNameStringData() != fieldName) {
                    auto allocatedElem =
                        _allocator->allocate(elem.type(), fieldName, elem.valuesize());
                    memcpy(allocatedElem.value(), elem.value(), elem.valuesize());
                    elem = allocatedElem.element();
                    state.lastValue = elem;
                }
            }

            // If the encoded element wasn't stored in the allocator above we need to copy it here
            // as we're building a full BSONObj.
            if (!elem.eoo()) {
                if (_allocator->position() == allocatorPosition) {
                    auto size = elem.size();
                    memcpy(_allocator->allocate(size), elem.rawdata(), size);
                }

                // Remember last known value, needed for further decompression.
                state.lastValue = elem;
            }

            ++processed;
            return true;
        });

    // Traverse interleaved reference object, we will match interleaved states with literals.
    auto res = t.traverse(interleaved.referenceObj);
    if (!res) {
        // Exit interleaved mode and load as regular. Re-instantiate the state and set last known
        // value.
        uassert(6067604, "Invalid BSON Column interleaved encoding", processed == 0);
        // This invalidates 'interleaved' reference, may no longer be dereferenced.
        Regular& regular = _mode.emplace<Regular>();
        get<0>(regular.state.decoder).deltaOfDelta = false;
        regular.state.lastValue = _decompressed;
        _incrementRegular(regular);
        return;
    }

    // There should have been as many interleaved states as scalar fields.
    uassert(6067605, "Invalid BSON Column interleaved encoding", stateIt == stateEnd);

    // Store built BSONObj in the decompressed list
    auto [objdata, objsize] = contiguous.done();

    // If no data was added, use a EOO literal. As buffer size is 0 we cannot interpret it as BSON.
    if (objsize == 0) {
        _decompressed = BSONElement();
        return;
    }

    // Root objects always have an empty field name and we already know the total object size.
    BSONElement obj(objdata, 1, objsize);

    _decompressed = obj;
}

void BSONColumn::Iterator::_handleEOO() {
    ++_control;
    uassert(7482200, "Invalid BSONColumn encoding", _control == _end);
    _index = kEndIndex;
    _decompressed = {};
}

bool BSONColumn::Iterator::_isLiteral(char control) {
    return (control & 0xE0) == 0;
}

bool BSONColumn::Iterator::_isInterleavedStart(char control) {
    return control == bsoncolumn::kInterleavedStartControlByteLegacy ||
        control == bsoncolumn::kInterleavedStartControlByte ||
        control == bsoncolumn::kInterleavedStartArrayRootControlByte;
}

uint8_t BSONColumn::Iterator::_numSimple8bBlocks(char control) {
    return (control & 0x0F) + 1;
}

void BSONColumn::Iterator::DecodingState::loadUncompressed(const BSONElement& elem) {
    BSONType type = elem.type();
    if (uses128bit(type)) {
        auto& d128 = decoder.emplace<Decoder128>();
        switch (type) {
            case String:
            case Code:
                d128.lastEncodedValue =
                    Simple8bTypeUtil::encodeString(elem.valueStringData()).value_or(0);
                break;
            case BinData: {
                int size;
                const char* binary = elem.binData(size);
                d128.lastEncodedValue = Simple8bTypeUtil::encodeBinary(binary, size).value_or(0);
                break;
            }
            case NumberDecimal:
                d128.lastEncodedValue = Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal());
                break;
            default:
                MONGO_UNREACHABLE;
        };
    } else {
        auto& d64 = decoder.emplace<Decoder64>();
        d64.deltaOfDelta = usesDeltaOfDelta(type);
        switch (type) {
            case jstOID:
                d64.lastEncodedValue = Simple8bTypeUtil::encodeObjectId(elem.__oid());
                break;
            case Date:
                d64.lastEncodedValue = elem.date().toMillisSinceEpoch();
                break;
            case Bool:
                d64.lastEncodedValue = elem.boolean();
                break;
            case NumberInt:
                d64.lastEncodedValue = elem._numberInt();
                break;
            case NumberLong:
                d64.lastEncodedValue = elem._numberLong();
                break;
            case bsonTimestamp:
                d64.lastEncodedValue = elem.timestampValue();
                break;
            default:
                break;
        };
        if (d64.deltaOfDelta) {
            d64.lastEncodedValueForDeltaOfDelta = d64.lastEncodedValue;
            d64.lastEncodedValue = 0;
        }
    }

    lastValue = elem;
}

BSONColumn::Iterator::DecodingState::LoadControlResult
BSONColumn::Iterator::DecodingState::loadControl(ElementStorage& allocator,
                                                 const char* buffer,
                                                 const char* end) {
    // Load current control byte, it can be either a literal or Simple-8b deltas
    uint8_t control = *buffer;
    if (_isLiteral(control)) {
        // Load BSONElement from the literal and set last encoded in case we need to calculate
        // deltas from this literal
        BSONElement literalElem(buffer, 1, -1);
        loadUncompressed(literalElem);
        return {literalElem, literalElem.size()};
    }

    // Setup decoder for this range of Simple-8b blocks
    uint8_t blocks = _numSimple8bBlocks(control);
    int size = sizeof(uint64_t) * blocks;
    uassert(6067608, "Invalid BSON Column encoding", buffer + size + 1 < end);

    // Instantiate decoder and load first value, every Simple-8b block should have at least one
    // value
    BSONElement deltaElem;
    stdx::visit(OverloadedVisitor{
                    [&](DecodingState::Decoder64& d64) {
                        // Simple-8b delta block, load its scale factor and validate for sanity
                        d64.scaleIndex = kControlToScaleIndex[(control & 0xF0) >> 4];
                        uassert(6067606,
                                "Invalid control byte in BSON Column",
                                d64.scaleIndex != kInvalidScaleIndex);

                        // If Double, scale last value according to this scale factor
                        auto type = lastValue.type();
                        if (type == NumberDouble) {
                            auto encoded = Simple8bTypeUtil::encodeDouble(lastValue._numberDouble(),
                                                                          d64.scaleIndex);
                            uassert(6067607, "Invalid double encoding in BSON Column", encoded);
                            d64.lastEncodedValue = *encoded;
                        }

                        // We can read the last known value from the decoder iterator even as it has
                        // reached end.
                        boost::optional<uint64_t> lastSimple8bValue =
                            d64.pos.valid() ? *d64.pos : 0;
                        d64.pos = Simple8b<uint64_t>(buffer + 1, size, lastSimple8bValue).begin();
                        deltaElem = loadDelta(allocator, d64);
                    },
                    [&](DecodingState::Decoder128& d128) {
                        // We can read the last known value from the decoder iterator even as it has
                        // reached end.
                        boost::optional<uint128_t> lastSimple8bValue =
                            d128.pos.valid() ? *d128.pos : uint128_t(0);
                        d128.pos = Simple8b<uint128_t>(buffer + 1, size, lastSimple8bValue).begin();
                        deltaElem = loadDelta(allocator, d128);
                    }},
                decoder);

    return {deltaElem, size + 1};
}

BSONElement BSONColumn::Iterator::DecodingState::loadDelta(ElementStorage& allocator,
                                                           Decoder64& d64) {
    const auto& delta = *d64.pos;
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        return BSONElement();
    }

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (!d64.deltaOfDelta && *delta == 0) {
        return lastValue;
    }

    // Expand delta or delta-of-delta as last encoded.
    d64.lastEncodedValue = expandDelta(d64.lastEncodedValue, Simple8bTypeUtil::decodeInt64(*delta));
    if (d64.deltaOfDelta) {
        d64.lastEncodedValueForDeltaOfDelta =
            expandDelta(d64.lastEncodedValueForDeltaOfDelta, d64.lastEncodedValue);
    }

    // Decoder state is now setup, materialize new value. We allocate a new BSONElement that fits
    // same value size as previous
    BSONType type = lastValue.type();
    ElementStorage::Element elem =
        allocator.allocate(type, lastValue.fieldNameStringData(), lastValue.valuesize());

    // Write value depending on type
    int64_t valueToWrite =
        d64.deltaOfDelta ? d64.lastEncodedValueForDeltaOfDelta : d64.lastEncodedValue;
    switch (type) {
        case NumberDouble:
            DataView(elem.value())
                .write<LittleEndian<double>>(
                    Simple8bTypeUtil::decodeDouble(valueToWrite, d64.scaleIndex));
            break;
        case jstOID: {
            Simple8bTypeUtil::decodeObjectIdInto(
                elem.value(), valueToWrite, lastValue.__oid().getInstanceUnique());
        } break;
        case Date:
        case NumberLong:
            DataView(elem.value()).write<LittleEndian<long long>>(valueToWrite);
            break;
        case Bool:
            DataView(elem.value()).write<LittleEndian<char>>(valueToWrite);
            break;
        case NumberInt:
            DataView(elem.value()).write<LittleEndian<int>>(valueToWrite);
            break;
        case bsonTimestamp: {
            DataView(elem.value()).write<LittleEndian<long long>>(valueToWrite);
        } break;
        case RegEx:
        case DBRef:
        case CodeWScope:
        case Symbol:
        case Object:
        case Array:
        case EOO:  // EOO indicates the end of an interleaved object.
            uasserted(6785500, "Invalid delta in BSON Column encoding");
        default:
            // No other types use int64 and need to allocate value storage
            MONGO_UNREACHABLE;
    }

    lastValue = elem.element();
    return lastValue;
}

BSONElement BSONColumn::Iterator::DecodingState::loadDelta(ElementStorage& allocator,
                                                           Decoder128& d128) {
    const auto& delta = *d128.pos;
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        return BSONElement();
    }

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (*delta == 0) {
        return lastValue;
    }

    // Expand delta as last encoded.
    d128.lastEncodedValue =
        expandDelta(d128.lastEncodedValue, Simple8bTypeUtil::decodeInt128(*delta));

    // Decoder state is now setup, write value depending on type
    auto elemFn = [&]() -> ElementStorage::Element {
        BSONType type = lastValue.type();
        switch (type) {
            case String:
            case Code: {
                Simple8bTypeUtil::SmallString ss =
                    Simple8bTypeUtil::decodeString(d128.lastEncodedValue);
                // Add 5 bytes to size, strings begin with a 4 byte count and ends with a null
                // terminator
                auto elem = allocator.allocate(type, lastValue.fieldNameStringData(), ss.size + 5);
                // Write count, size includes null terminator
                DataView(elem.value()).write<LittleEndian<int32_t>>(ss.size + 1);
                // Write string value
                memcpy(elem.value() + sizeof(int32_t), ss.str.data(), ss.size);
                // Write null terminator
                DataView(elem.value()).write<char>('\0', ss.size + sizeof(int32_t));
                return elem;
            }
            case BinData: {
                auto elem = allocator.allocate(
                    type, lastValue.fieldNameStringData(), lastValue.valuesize());
                // The first 5 bytes in binData is a count and subType, copy them from previous
                memcpy(elem.value(), lastValue.value(), 5);
                Simple8bTypeUtil::decodeBinary(
                    d128.lastEncodedValue, elem.value() + 5, lastValue.valuestrsize());
                return elem;
            }
            case NumberDecimal: {
                auto elem = allocator.allocate(
                    type, lastValue.fieldNameStringData(), lastValue.valuesize());
                Decimal128 dec128 = Simple8bTypeUtil::decodeDecimal128(d128.lastEncodedValue);
                Decimal128::Value dec128Val = dec128.getValue();
                DataView(elem.value()).write<LittleEndian<long long>>(dec128Val.low64);
                DataView(elem.value() + sizeof(long long))
                    .write<LittleEndian<long long>>(dec128Val.high64);
                return elem;
            }
            default:
                // No other types should use int128
                MONGO_UNREACHABLE;
        }
    }();

    lastValue = elemFn.element();
    return lastValue;
}

BSONColumn::Iterator::Interleaved::Interleaved(BSONObj refObj,
                                               BSONType referenceObjType,
                                               bool interleavedArrays)
    : referenceObj(std::move(refObj)), arrays(interleavedArrays), rootType(referenceObjType) {}

BSONColumn::BSONColumn(const char* buffer, size_t size)
    : _binary(buffer), _size(size), _allocator(new ElementStorage()) {
    _initialValidate();
}

BSONColumn::BSONColumn(BSONElement bin) {
    tassert(5857700,
            "Invalid BSON type for column",
            bin.type() == BSONType::BinData && bin.binDataType() == BinDataType::Column);

    _binary = bin.binData(_size);
    _allocator = new ElementStorage();
    _initialValidate();
}

BSONColumn::BSONColumn(BSONBinData bin)
    : BSONColumn(static_cast<const char*>(bin.data), bin.length) {
    tassert(6179300, "Invalid BSON type for column", bin.type == BinDataType::Column);
}

void BSONColumn::_initialValidate() {
    uassert(6067609, "Invalid BSON Column encoding", _size > 0);
}

BSONColumn::Iterator BSONColumn::begin() const {
    return {_allocator, _binary, _binary + _size};
}

BSONColumn::Iterator BSONColumn::end() const {
    return {};
}

boost::optional<BSONElement> BSONColumn::operator[](size_t index) const {
    // Traverse until we reach desired index or end
    auto it = begin();
    auto e = end();
    for (size_t i = 0; it != e && i < index; ++it, ++i) {
    }

    // Return none if out of bounds
    if (it == e) {
        return boost::none;
    }

    return *it;
}

size_t BSONColumn::size() const {
    return std::distance(begin(), end());
}

bool BSONColumn::contains_forTest(BSONType elementType) const {
    const char* byteIter = _binary;
    const char* columnEnd = _binary + _size;

    uint8_t control;
    while (byteIter != columnEnd) {
        control = static_cast<uint8_t>(*byteIter);
        if (Iterator::_isLiteral(control)) {
            BSONElement literalElem(byteIter, 1, -1);
            if (control == elementType) {
                return true;
            } else if (control == BSONType::EOO) {
                // TODO: check for valid encoding
                // reached end of column
                return false;
            }

            byteIter += literalElem.size();
        } else if (Iterator::_isInterleavedStart(*byteIter)) {

            // TODO SERVER-74926 add interleaved support
            uasserted(6580401,
                      "Interleaved mode not yet supported for BSONColumn::contains_forTest.");
        } else { /* Simple-8b Delta Block */
            uint8_t numBlocks = Iterator::_numSimple8bBlocks(control);
            int simple8bBlockSize = sizeof(uint64_t) * numBlocks;
            uassert(
                6580400, "Invalid BSON Column encoding", byteIter + simple8bBlockSize < columnEnd);

            // skip simple8b control blocks
            byteIter += simple8bBlockSize;
        }
    }

    return false;
}

boost::intrusive_ptr<BSONColumn::ElementStorage> BSONColumn::release() {
    auto previous = _allocator;
    _allocator = new ElementStorage();
    return previous;
}

}  // namespace mongo
