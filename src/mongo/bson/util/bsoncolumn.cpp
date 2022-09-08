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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/simple8b_type_util.h"

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

// Sentinel to indicate end index for BSONColumn Iterator.
constexpr size_t kEndIndex = 0xFFFFFFFFFFFFFFFF;

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
    return {_buffer, _nameSize + 1, _valueSize + _nameSize + kElementValueOffset};
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

const char* BSONColumn::ElementStorage::ContiguousBlock::done() {
    auto ptr = _storage.contiguous();
    _storage._endContiguous();
    _finished = true;
    return ptr;
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

void BSONColumn::ElementStorage::_endContiguous() {
    _contiguousEnabled = false;
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

BSONColumn::Iterator::Iterator(BSONColumn& column, const char* pos, const char* end)
    : _column(&column), _control(pos), _end(end) {}

void BSONColumn::Iterator::_initialize(size_t index) {
    _index = index;

    if (_isInterleavedStart(*_control)) {
        _initializeInterleaving();
        return;
    }

    const BSONElement* current = nullptr;
    if (index < _column->_decompressed.size()) {
        current = &_column->_decompressed[index];
        _state._lastValue = *current;
    }
    // If we are at EOO then start at end.
    if (*_control == EOO) {
        _handleEOO();
        return;
    }

    // previous doesn't matter when we load literals
    auto result = _state._loadControl(*_column, _control, _end, current);
    if (!current) {
        _column->_decompressed.push_back(result.element);
    }
    _control += result.size;
}

void BSONColumn::Iterator::_initializeInterleaving() {
    _interleavedArrays = *_control == bsoncolumn::kInterleavedStartControlByte ||
        *_control == bsoncolumn::kInterleavedStartArrayRootControlByte;
    _interleavedRootType =
        *_control == bsoncolumn::kInterleavedStartArrayRootControlByte ? Array : Object;
    _interleavedReferenceObj = BSONObj(_control + 1);

    BSONObjTraversal t(_interleavedArrays,
                       _interleavedRootType,
                       [](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
                       [this](const BSONElement& elem) {
                           _states.emplace_back();
                           _states.back()._loadLiteral(elem);
                           return true;
                       });
    t.traverse(_interleavedReferenceObj);
    uassert(6067610, "Invalid BSONColumn encoding", !_states.empty());

    _control += _interleavedReferenceObj.objsize() + 1;
    _incrementInterleaved();
}

BSONColumn::Iterator& BSONColumn::Iterator::operator++() {
    // We need to setup iterator state even if this is not the first time we iterate in case we need
    // to decompress elements further along
    ++_index;

    if (_states.empty()) {
        _incrementRegular();
    } else {
        _incrementInterleaved();
    }

    return *this;
}

BSONColumn::Iterator BSONColumn::Iterator::operator++(int) {
    auto ret = *this;
    operator++();
    return ret;
}

void BSONColumn::Iterator::_incrementRegular() {
    DecodingState& state = _state;

    // Get pointer to current element if we are already decompressed
    const BSONElement* current =
        _index < _column->_decompressed.size() ? &_column->_decompressed[_index] : nullptr;
    // Traverse current Simple8b block for 64bit values if it exists
    if (state._decoder64 && ++state._decoder64->pos != state._decoder64->end) {
        auto elem = state._loadDelta(*_column, *state._decoder64->pos, current);
        if (!current) {
            _column->_decompressed.emplace_back(elem);
        }
        return;
    }

    // Traverse current Simple8b block for 128bit values if it exists
    if (state._decoder128 && ++state._decoder128->pos != state._decoder128->end) {
        auto elem = state._loadDelta(*_column, *state._decoder128->pos, current);
        if (!current) {
            _column->_decompressed.emplace_back(elem);
        }
        return;
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
        // Remember this position to speed up "random access" for further access.
        _column->_maxDecodingStartPos.setIfLarger(_index, _control);

        _initializeInterleaving();
        return;
    }
    auto result = state._loadControl(*_column, _control, _end, current);
    if (!current) {
        _column->_decompressed.emplace_back(result.element);
    }
    auto prevControl = _control;
    _control += result.size;
    if (result.full) {
        // Remember this position to speed up "random access" for further access.
        _column->_maxDecodingStartPos.setIfLarger(_index, prevControl);
    }
}
void BSONColumn::Iterator::_incrementInterleaved() {
    // Get pointer to current element if we are already decompressed
    const BSONElement* current =
        _index < _column->_decompressed.size() ? &_column->_decompressed[_index] : nullptr;

    // Notify the internal allocator to keep all allocations in contigous memory. That way we can
    // produce the full BSONObj that we need to return.
    auto contiguous = _column->_elementStorage.startContiguous();

    // Iterate over the reference interleaved object. We match scalar subfields with our interleaved
    // states in order. Internally the necessary recursion is performed and the second lambda below
    // is called for scalar fields. Every element writes its data to the allocator so a full BSONObj
    // is produced, this usually happens within _loadDelta() but must explicitly be done in the
    // cases where re-materialization of the Element wasn't required (same as previous for example).
    // The first lambda outputs an RAII object that is instantiated every time we recurse deeper.
    // This handles writing the BSONObj size and EOO bytes for subobjects.
    auto stateIt = _states.begin();
    auto stateEnd = _states.end();
    int processed = 0;
    BSONObjTraversal t(
        _interleavedArrays,
        _interleavedRootType,
        [this](StringData fieldName, const BSONObj& obj, BSONType type) {
            // Called every time we recurse into a subobject. It makes sure we write the size and
            // EOO bytes.
            return SubObjectAllocator(_column->_elementStorage, fieldName, obj, type);
        },
        [this, &stateIt, &stateEnd, &processed](const BSONElement& referenceField) {
            // Called for every scalar field in the reference interleaved BSONObj. We have as many
            // decoding states as scalars.
            uassert(6067603, "Invalid BSON Column interleaved encoding", stateIt != stateEnd);
            auto& state = *(stateIt++);

            // Remember the iterator position before writing anything. This is to detect that
            // nothing was written and we need to copy the element into the allocator position.
            auto allocatorPosition = _column->_elementStorage.position();
            BSONElement elem;
            // Load deltas if decoders are setup. nullptr is always used for "current". So even if
            // we are iterating the second time we are going to allocate new memory. This is a
            // tradeoff to avoid a decoded list of literals for every state that will only be used
            // if we iterate multiple times.
            if (state._decoder64 && ++state._decoder64->pos != state._decoder64->end) {
                elem = state._loadDelta(*_column, *state._decoder64->pos, nullptr);
            } else if (state._decoder128 && ++state._decoder128->pos != state._decoder128->end) {
                elem = state._loadDelta(*_column, *state._decoder128->pos, nullptr);
            } else if (*_control == EOO) {
                // Decoders are exhausted and the next control byte was EOO then we should exit
                // interleaved mode. Return false to end the recursion early.
                ++_control;
                return false;
            } else {
                // Decoders are exhausted so we need to load the next control byte that by
                // definition belong to this decoder state as we iterate in the same known order.
                auto result = state._loadControl(*_column, _control, _end, nullptr);
                _control += result.size;
                elem = result.element;

                // If the loaded control byte was a literal it is stored without field name. We need
                // to create a new BSONElement with the field name added as this is a sub-field in
                // an object.
                auto fieldName = referenceField.fieldNameStringData();
                if (!elem.eoo() && elem.fieldNameStringData() != fieldName) {
                    auto allocatedElem =
                        _column->_elementStorage.allocate(elem.type(), fieldName, elem.valuesize());
                    memcpy(allocatedElem.value(), elem.value(), elem.valuesize());
                    elem = allocatedElem.element();
                    state._lastValue = elem;
                }
            }

            // If the encoded element wasn't stored in the allocator above we need to copy it here
            // as we're building a full BSONObj.
            if (!elem.eoo()) {
                if (_column->_elementStorage.position() == allocatorPosition) {
                    auto size = elem.size();
                    memcpy(_column->_elementStorage.allocate(size), elem.rawdata(), size);
                }

                // Remember last known value, needed for further decompression.
                state._lastValue = elem;
            }

            ++processed;
            return true;
        });

    // Traverse interleaved reference object, we will match interleaved states with literals.
    auto res = t.traverse(_interleavedReferenceObj);
    if (!res) {
        // Exit interleaved mode and load as regular. Re-instantiate the state and set last known
        // value.
        _states.clear();
        uassert(6067604, "Invalid BSON Column interleaved encoding", processed == 0);
        _state = {};
        _state._lastValue = _column->_decompressed[_index - 1];

        _incrementRegular();
        return;
    }

    // There should have been as many interleaved states as scalar fields.
    uassert(6067605, "Invalid BSON Column interleaved encoding", stateIt == stateEnd);

    // If this element has been decompressed in a previous iteration we don't need to store it in
    // our decompressed list.
    if (current) {
        return;
    }

    // Store built BSONObj in the decompressed list
    const char* objdata = contiguous.done();
    BSONElement obj(objdata);

    // If no data was added, use a EOO literal instead of an empty object.
    if (obj.objsize() == 0) {
        obj = BSONElement();
    }

    _column->_decompressed.emplace_back(obj);
}

void BSONColumn::Iterator::_handleEOO() {
    ++_control;
    _index = kEndIndex;
    _column->_fullyDecompressed = true;
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

bool BSONColumn::Iterator::operator==(const Iterator& rhs) const {
    return _index == rhs._index;
}
bool BSONColumn::Iterator::operator!=(const Iterator& rhs) const {
    return !operator==(rhs);
}

BSONColumn::Iterator BSONColumn::Iterator::moveTo(BSONColumn& column) {
    auto copy = *this;
    _column = nullptr;
    copy._column = &column;
    return copy;
}

void BSONColumn::Iterator::DecodingState::_loadLiteral(const BSONElement& elem) {
    _lastType = elem.type();
    _deltaOfDelta = usesDeltaOfDelta(_lastType);
    switch (_lastType) {
        case String:
        case Code:
            _lastEncodedValue128 =
                Simple8bTypeUtil::encodeString(elem.valueStringData()).value_or(0);
            break;
        case BinData: {
            int size;
            const char* binary = elem.binData(size);
            _lastEncodedValue128 = Simple8bTypeUtil::encodeBinary(binary, size).value_or(0);
            break;
        }
        case jstOID:
            _lastEncodedValue64 = Simple8bTypeUtil::encodeObjectId(elem.__oid());
            break;
        case Date:
            _lastEncodedValue64 = elem.date().toMillisSinceEpoch();
            break;
        case Bool:
            _lastEncodedValue64 = elem.boolean();
            break;
        case NumberInt:
            _lastEncodedValue64 = elem._numberInt();
            break;
        case NumberLong:
            _lastEncodedValue64 = elem._numberLong();
            break;
        case bsonTimestamp:
            _lastEncodedValue64 = elem.timestampValue();
            break;
        case NumberDecimal:
            _lastEncodedValue128 = Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal());
            break;
        default:
            break;
    };
    if (_deltaOfDelta) {
        _lastEncodedValueForDeltaOfDelta = _lastEncodedValue64;
        _lastEncodedValue64 = 0;
    }
    _lastValue = elem;
}

BSONColumn::Iterator::DecodingState::LoadControlResult
BSONColumn::Iterator::DecodingState::_loadControl(BSONColumn& column,
                                                  const char* buffer,
                                                  const char* end,
                                                  const BSONElement* current) {
    // Load current control byte, it can be either a literal or Simple-8b deltas
    uint8_t control = *buffer;
    if (_isLiteral(control)) {
        // Load BSONElement from the literal and set last encoded in case we need to calculate
        // deltas from this literal
        BSONElement literalElem(buffer, 1, -1);
        _loadLiteral(literalElem);

        _decoder64 = boost::none;
        _decoder128 = boost::none;
        _lastValue = literalElem;

        return {literalElem, literalElem.size(), true};
    }

    // Simple-8b delta block, load its scale factor and validate for sanity
    _scaleIndex = kControlToScaleIndex[(control & 0xF0) >> 4];
    uassert(6067606, "Invalid control byte in BSON Column", _scaleIndex != kInvalidScaleIndex);

    // If Double, scale last value according to this scale factor
    auto type = _lastValue.type();
    if (type == NumberDouble) {
        auto encoded = Simple8bTypeUtil::encodeDouble(_lastValue._numberDouble(), _scaleIndex);
        uassert(6067607, "Invalid double encoding in BSON Column", encoded);
        _lastEncodedValue64 = *encoded;
    }

    // Setup decoder for this range of Simple-8b blocks
    uint8_t blocks = _numSimple8bBlocks(control);
    int size = sizeof(uint64_t) * blocks;
    uassert(6067608, "Invalid BSON Column encoding", buffer + size + 1 < end);

    // Instantiate decoder and load first value, every Simple-8b block should have at least one
    // value
    BSONElement deltaElem;
    if (!uses128bit(type)) {
        // We can read the last known value from the decoder iterator even as it has reached end.
        boost::optional<uint64_t> lastSimple8bValue = _decoder64 ? *_decoder64->pos : 0;
        _decoder64.emplace(buffer + 1, size, lastSimple8bValue);
        deltaElem = _loadDelta(column, *_decoder64->pos, current);
    } else {
        // We can read the last known value from the decoder iterator even as it has reached end.
        boost::optional<uint128_t> lastSimple8bValue =
            _decoder128 ? *_decoder128->pos : uint128_t(0);
        _decoder128.emplace(buffer + 1, size, lastSimple8bValue);
        deltaElem = _loadDelta(column, *_decoder128->pos, current);
    }

    return {deltaElem, size + 1, false};
}

BSONElement BSONColumn::Iterator::DecodingState::_loadDelta(BSONColumn& column,
                                                            const boost::optional<uint64_t>& delta,
                                                            const BSONElement* current) {
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        return BSONElement();
    }

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (!_deltaOfDelta && *delta == 0) {
        return _lastValue;
    }

    // Expand delta or delta-of-delta as last encoded.
    _lastEncodedValue64 = expandDelta(_lastEncodedValue64, Simple8bTypeUtil::decodeInt64(*delta));
    if (_deltaOfDelta) {
        _lastEncodedValueForDeltaOfDelta =
            expandDelta(_lastEncodedValueForDeltaOfDelta, _lastEncodedValue64);
    }

    // Decoder state is now setup, no need to create BSONElement if already exist decompressed,
    // return dummy EOO element.
    if (current) {
        _lastValue = *current;
        return *current;
    }
    // Allocate a new BSONElement that fits same value size as previous
    ElementStorage::Element elem = column._elementStorage.allocate(
        _lastType, _lastValue.fieldNameStringData(), _lastValue.valuesize());

    // Write value depending on type
    int64_t valueToWrite = _deltaOfDelta ? _lastEncodedValueForDeltaOfDelta : _lastEncodedValue64;
    switch (_lastType) {
        case NumberDouble:
            DataView(elem.value())
                .write<LittleEndian<double>>(
                    Simple8bTypeUtil::decodeDouble(valueToWrite, _scaleIndex));
            break;
        case jstOID: {
            Simple8bTypeUtil::decodeObjectIdInto(
                elem.value(), valueToWrite, _lastValue.__oid().getInstanceUnique());
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

    _lastValue = elem.element();
    return _lastValue;
}

BSONElement BSONColumn::Iterator::DecodingState::_loadDelta(BSONColumn& column,
                                                            const boost::optional<uint128_t>& delta,
                                                            const BSONElement* current) {
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        return BSONElement();
    }

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (*delta == 0) {
        return _lastValue;
    }

    // Expand delta as last encoded.
    _lastEncodedValue128 =
        expandDelta(_lastEncodedValue128, Simple8bTypeUtil::decodeInt128(*delta));

    // Decoder state is now setup, no need to create BSONElement if already exist decompressed,
    // return dummy EOO element.
    if (current) {
        _lastValue = *current;
        return *current;
    }

    // Write value depending on type
    auto elem = [&]() -> ElementStorage::Element {
        switch (_lastType) {
            case String:
            case Code: {
                Simple8bTypeUtil::SmallString ss =
                    Simple8bTypeUtil::decodeString(_lastEncodedValue128);
                // Add 5 bytes to size, strings begin with a 4 byte count and ends with a null
                // terminator
                auto elem = column._elementStorage.allocate(
                    _lastType, _lastValue.fieldNameStringData(), ss.size + 5);
                // Write count, size includes null terminator
                DataView(elem.value()).write<LittleEndian<int32_t>>(ss.size + 1);
                // Write string value
                memcpy(elem.value() + sizeof(int32_t), ss.str.data(), ss.size);
                // Write null terminator
                DataView(elem.value()).write<char>('\0', ss.size + sizeof(int32_t));
                return elem;
            }
            case BinData: {
                auto elem = column._elementStorage.allocate(
                    _lastType, _lastValue.fieldNameStringData(), _lastValue.valuesize());
                // The first 5 bytes in binData is a count and subType, copy them from previous
                memcpy(elem.value(), _lastValue.value(), 5);
                Simple8bTypeUtil::decodeBinary(
                    _lastEncodedValue128, elem.value() + 5, _lastValue.valuestrsize());
                return elem;
            }
            case NumberDecimal: {
                auto elem = column._elementStorage.allocate(
                    _lastType, _lastValue.fieldNameStringData(), _lastValue.valuesize());
                Decimal128 d128 = Simple8bTypeUtil::decodeDecimal128(_lastEncodedValue128);
                Decimal128::Value d128Val = d128.getValue();
                DataView(elem.value()).write<LittleEndian<long long>>(d128Val.low64);
                DataView(elem.value() + sizeof(long long))
                    .write<LittleEndian<long long>>(d128Val.high64);
                return elem;
            }
            default:
                // No other types should use int128
                MONGO_UNREACHABLE;
        }
    }();

    _lastValue = elem.element();
    return _lastValue;
}

BSONColumn::BSONColumn(BSONElement bin) {
    tassert(5857700,
            "Invalid BSON type for column",
            bin.type() == BSONType::BinData && bin.binDataType() == BinDataType::Column);

    _binary = bin.binData(_size);
    _name = bin.fieldNameStringData().toString();
    _init();
}

BSONColumn::BSONColumn(BSONBinData bin, StringData name) {
    tassert(6179300, "Invalid BSON type for column", bin.type == BinDataType::Column);
    _binary = static_cast<const char*>(bin.data);
    _size = bin.length;
    _name = name.toString();
    _init();
}

void BSONColumn::_init() {
    uassert(6067609, "Invalid BSON Column encoding", _size > 0);
    _maxDecodingStartPos._control = _binary;
}

BSONColumn::Iterator BSONColumn::begin() {
    Iterator it{*this, _binary, _binary + _size};
    it._initialize(0);
    return it;
}

BSONColumn::Iterator BSONColumn::end() {
    Iterator it{*this, _binary + _size, _binary + _size};
    it._index = kEndIndex;
    return it;
}

boost::optional<const BSONElement&> BSONColumn::operator[](size_t index) {
    // If index is already decompressed, we can just return the element
    if (index < _decompressed.size()) {
        return _decompressed[index];
    }

    // No more elements to be found if we are fully decompressed, return EOO
    if (_fullyDecompressed)
        return boost::none;

    // We can begin iterating from last known literal
    Iterator it{*this, _maxDecodingStartPos._control, _binary + _size};
    it._initialize(_maxDecodingStartPos._index);

    // Traverse until we reach desired index or end
    auto e = end();
    for (size_t i = _maxDecodingStartPos._index; it != e && i < index; ++it, ++i) {
    }

    // Return EOO if not found
    if (it == e)
        return boost::none;

    return *it;
}

size_t BSONColumn::size() {
    if (_fullyDecompressed)
        return _decompressed.size();

    // We can begin iterating from last known literal
    Iterator it{*this, _maxDecodingStartPos._control, _binary + _size};
    it._initialize(_maxDecodingStartPos._index);

    // Traverse until we reach end
    for (auto e = end(); it != e; ++it) {
    }

    invariant(_fullyDecompressed);
    return _decompressed.size();
}

void BSONColumn::DecodingStartPosition::setIfLarger(size_t index, const char* control) {
    if (_index < index) {
        _control = control;
        _index = index;
    }
}

}  // namespace mongo
