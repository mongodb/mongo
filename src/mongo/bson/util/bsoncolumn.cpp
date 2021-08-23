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
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/simple8b_type_util.h"

#include <algorithm>

namespace mongo {
using namespace bsoncolumn;

namespace {
// Start capacity for memory blocks allocated by ElementStorage
constexpr int kStartCapacity = 128;

// Max capacity for memory blocks allocated by ElementStorage
constexpr int kMaxCapacity = 1024 * 32;

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

}  // namespace

BSONColumn::ElementStorage::Element::Element(char* buffer, int valueSize)
    : _buffer(buffer), _valueSize(valueSize) {}

char* BSONColumn::ElementStorage::Element::value() {
    // Skip over type byte and null terminator for field name
    return _buffer + kElementValueOffset;
}

int BSONColumn::ElementStorage::Element::size() const {
    return _valueSize;
}

BSONElement BSONColumn::ElementStorage::Element::element() const {
    return {_buffer, 1, _valueSize + kElementValueOffset, BSONElement::CachedSizeTag{}};
}

BSONColumn::ElementStorage::Element BSONColumn::ElementStorage::allocate(BSONType type,
                                                                         int valueSize) {
    // Size needed for this BSONElement
    int size = valueSize + kElementValueOffset;

    // If current block don't have enough capacity we need to allocate a new one
    if (_capacity - _pos < size) {
        // Keep track of current block if it exists.
        if (_block) {
            _blocks.push_back(std::move(_block));
        }

        // Double block size while keeping it within [kStartCapacity, kMaxCapacity] range, unless a
        // size larger than kMaxCapacity is requested.
        _capacity = std::max(std::clamp(_capacity * 2, kStartCapacity, kMaxCapacity), size);
        _block = std::make_unique<char[]>(_capacity);
        _pos = 0;
    }

    // Write type and null terminator in the first two bytes
    _block[_pos] = type;
    _block[_pos + 1] = '\0';

    // Construct the Element, current block will have enough size at this point
    Element elem(_block.get() + _pos, valueSize);

    // Increment the used size and return
    _pos += size;
    return elem;
}

BSONColumn::Iterator::Iterator(BSONColumn& column, const char* pos, const char* end)
    : _column(column), _control(pos), _end(end) {}

BSONColumn::Iterator& BSONColumn::Iterator::operator++() {
    // We need to setup iterator state even if this is not the first time we iterate in case we need
    // to decompress elements further along
    ++_index;

    // Get reference to last non-skipped element. Needed to apply delta.
    const auto& lastVal = _column._decompressed.at(_lastValueIndex);

    // Traverse current Simple8b block for 64bit values if it exists
    if (_decoder64 && ++_decoder64->pos != _decoder64->end) {
        _loadDelta(lastVal, *_decoder64->pos);
        return *this;
    }

    // Traverse current Simple8b block for 128bit values if it exists
    if (_decoder128 && ++_decoder128->pos != _decoder128->end) {
        _loadDelta(lastVal, *_decoder128->pos);
        return *this;
    }

    // We don't have any more delta values in current block so we need to move to the next control
    // byte.
    if (_literal(*_control)) {
        // If we were positioned on a literal, move to the byte after
        _control += lastVal.size();
    } else {
        // If we were positioned on Simple-8b blocks, move to the byte after then
        uint8_t blocks = (*_control & 0x0F) + 1;
        _control += sizeof(uint64_t) * blocks + 1;
    }

    // Validate that we are not reading out of bounds
    uassert(ErrorCodes::BadValue, "Invalid BSON Column encoding", _control < _end);

    // Load new control byte
    _loadControl(lastVal);

    return *this;
}

BSONColumn::Iterator BSONColumn::Iterator::operator++(int) {
    auto ret = *this;
    operator++();
    return ret;
}

bool BSONColumn::Iterator::operator==(const Iterator& rhs) const {
    return _index == rhs._index;
}
bool BSONColumn::Iterator::operator!=(const Iterator& rhs) const {
    return !operator==(rhs);
}

void BSONColumn::Iterator::_loadControl(const BSONElement& prev) {
    // Load current control byte, it can be either a literal or Simple-8b deltas
    uint8_t control = *_control;
    if (_literal(control)) {
        // If we detect EOO we are done, set values to be the same as an end iterator
        if (control == EOO) {
            _control += 1;
            _index = kEndIndex;
            _column._fullyDecompressed = true;
            return;
        }

        // Load BSONElement from the literal and set last encoded in case we need to calculate
        // deltas from this literal
        BSONElement literalElem(_control, 1, -1, BSONElement::CachedSizeTag{});
        switch (literalElem.type()) {
            case BinData: {
                int size;
                const char* binary = literalElem.binData(size);
                if (size <= 16) {
                    _lastEncodedValue128 = Simple8bTypeUtil::encodeBinary(binary, size);
                }
                break;
            }
            case jstOID:
                _lastEncodedValue64 = Simple8bTypeUtil::encodeObjectId(literalElem.__oid());
                break;
            case Date:
                _lastEncodedValue64 = literalElem.date().toMillisSinceEpoch();
                break;
            case Bool:
                _lastEncodedValue64 = literalElem.boolean();
                break;
            case NumberInt:
                _lastEncodedValue64 = literalElem._numberInt();
                break;
            case NumberLong:
                _lastEncodedValue64 = literalElem._numberLong();
                break;
            case bsonTimestamp:
                _lastEncodedValue64 = 0;
                _lastEncodedValueForDeltaOfDelta = literalElem.timestamp().asULL();
                break;
            case NumberDecimal:
                _lastEncodedValue128 =
                    Simple8bTypeUtil::encodeDecimal128(literalElem._numberDecimal());
                break;
            default:
                break;
        };

        // Store at and and reset any previous decoders
        _storeElementIfNeeded(literalElem);

        _lastValueIndex = _index;
        _decoder64 = boost::none;
        _decoder128 = boost::none;

        // Remember index to last literal to speed up "random access".
        if (_column._indexLastLiteral < _index) {
            _column._controlLastLiteral = _control;
            _column._indexLastLiteral = _index;
        }

        return;
    }

    // Simple-8b delta block, load its scale factor and validate for sanity
    _scaleIndex = kControlToScaleIndex[(control & 0xF0) >> 4];
    uassert(ErrorCodes::BadValue,
            "Invalid control byte in BSON Column",
            _scaleIndex != kInvalidScaleIndex);

    // If Double, scale last value according to this scale factor
    auto type = prev.type();
    if (type == NumberDouble) {
        auto encoded = Simple8bTypeUtil::encodeDouble(prev._numberDouble(), _scaleIndex);
        uassert(ErrorCodes::BadValue, "Invalid double encoding in BSON Column", encoded);
        _lastEncodedValue64 = *encoded;
    }

    // Setup decoder for this range of Simple-8b blocks
    uint8_t blocks = _numSimple8bBlocks(control);
    auto size = sizeof(uint64_t) * blocks;
    uassert(ErrorCodes::BadValue, "Invalid BSON Column encoding", _control + size + 1 < _end);

    // Instantiate decoder and load first value, every Simple-8b block should have at least one
    // value
    if (!uses128bit(type)) {
        _decoder64.emplace(_control + 1, size);
        _loadDelta(prev, *_decoder64->pos);
    } else {
        _decoder128.emplace(_control + 1, size);
        _loadDelta(prev, *_decoder128->pos);
    }
}

void BSONColumn::Iterator::_loadDelta(const BSONElement& prev,
                                      const boost::optional<uint64_t>& delta) {
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        _storeElementIfNeeded(BSONElement());
        return;
    }

    // We have an actual value, remember this index for future delta lookups.
    _lastValueIndex = _index;
    BSONType type = prev.type();

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    bool deltaOfDelta = usesDeltaOfDelta(type);
    if (!deltaOfDelta && *delta == 0) {
        _storeElementIfNeeded(prev);
        return;
    }

    // Expand delta or delta-of-delta as last encoded.
    _lastEncodedValue64 = expandDelta(_lastEncodedValue64, Simple8bTypeUtil::decodeInt64(*delta));
    if (deltaOfDelta) {
        _lastEncodedValueForDeltaOfDelta =
            expandDelta(_lastEncodedValueForDeltaOfDelta, _lastEncodedValue64);
    }

    // Decoder state is now setup, no need to create BSONElement if already exist decompressed
    if (!_needStoreElement()) {
        return;
    }

    // Allocate a new BSONElement that fits same value size as previous
    ElementStorage::Element elem = _column._elementStorage.allocate(type, prev.valuesize());

    // Write value depending on type
    switch (type) {
        case NumberDouble:
            DataView(elem.value())
                .write<LittleEndian<double>>(
                    Simple8bTypeUtil::decodeDouble(_lastEncodedValue64, _scaleIndex));
            break;
        case jstOID: {
            Simple8bTypeUtil::decodeObjectIdInto(
                elem.value(), _lastEncodedValue64, prev.__oid().getInstanceUnique());
        } break;
        case Date:
        case NumberLong:
            DataView(elem.value()).write<LittleEndian<long long>>(_lastEncodedValue64);
            break;
        case Bool:
            DataView(elem.value()).write<LittleEndian<char>>(_lastEncodedValue64);
            break;
        case NumberInt:
            DataView(elem.value()).write<LittleEndian<int>>(_lastEncodedValue64);
            break;
        case bsonTimestamp: {
            DataView(elem.value()).write<LittleEndian<long long>>(_lastEncodedValueForDeltaOfDelta);
        } break;
        default:
            // unhandled for now
            break;
    }

    // Append our written BSONElement to decompressed values
    _column._decompressed.push_back(elem.element());
}

void BSONColumn::Iterator::_loadDelta(const BSONElement& prev,
                                      const boost::optional<uint128_t>& delta) {
    // boost::none represent skip, just append EOO BSONElement.
    if (!delta) {
        _storeElementIfNeeded(BSONElement());
        return;
    }

    // We have an actual value, remember this index for future delta lookups.
    _lastValueIndex = _index;
    BSONType type = prev.type();

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (*delta == 0) {
        _storeElementIfNeeded(prev);
        return;
    }

    // Expand delta as last encoded.
    _lastEncodedValue128 =
        expandDelta(_lastEncodedValue128, Simple8bTypeUtil::decodeInt128(*delta));

    // Decoder state is now setup, no need to create BSONElement if already exist decompressed
    if (!_needStoreElement()) {
        return;
    }

    // Allocate a new BSONElement that fits same value size as previous
    ElementStorage::Element elem = _column._elementStorage.allocate(type, prev.valuesize());

    // Write value depending on type
    switch (type) {
        case BinData:
            // The first 5 bytes in binData is a count and subType, copy them from previous
            memcpy(elem.value(), prev.value(), 5);
            Simple8bTypeUtil::decodeBinary(
                _lastEncodedValue128, elem.value() + 5, prev.valuestrsize());
            break;
        case NumberDecimal: {
            Decimal128 d128 = Simple8bTypeUtil::decodeDecimal128(_lastEncodedValue128);
            Decimal128::Value d128Val = d128.getValue();
            DataView(elem.value()).write<LittleEndian<long long>>(d128Val.low64);
            DataView(elem.value() + sizeof(long long))
                .write<LittleEndian<long long>>(d128Val.high64);
            break;
        }
        default:
            // unhandled for now
            break;
    }

    _column._decompressed.push_back(elem.element());
}

bool BSONColumn::Iterator::_needStoreElement() const {
    return _index == _column._decompressed.size();
}

void BSONColumn::Iterator::_storeElementIfNeeded(BSONElement elem) {
    if (_needStoreElement()) {
        _column._decompressed.emplace_back(elem);
    }
}

BSONColumn::BSONColumn(BSONElement bin) {
    tassert(5857700,
            "Invalid BSON type for column",
            bin.type() == BSONType::BinData && bin.binDataType() == BinDataType::Column);
    _binary = bin.binData(_size);
    uassert(ErrorCodes::BadValue, "Invalid BSON Column encoding", _size > kElementCountBytes);
    _elementCount = ConstDataView(_binary).read<LittleEndian<uint32_t>>();
    _controlLastLiteral = _binary + kElementCountBytes;
}

BSONColumn::Iterator BSONColumn::begin() {
    Iterator it{*this, _binary + kElementCountBytes, _binary + _size};
    it._loadControl(BSONElement());
    return it;
}

BSONColumn::Iterator BSONColumn::end() {
    Iterator it{*this, _binary + _size, _binary + _size};
    it._index = kEndIndex;
    return it;
}

BSONElement BSONColumn::operator[](size_t index) {
    // If index is already decompressed, we can just return the element
    if (index < _decompressed.size()) {
        return _decompressed[index];
    }

    // No more elements to be found if we are fully decompressed, return EOO
    if (_fullyDecompressed)
        return BSONElement();

    // We can begin iterating from last known literal
    Iterator it{*this, _controlLastLiteral, _binary + _size};
    it._index = _indexLastLiteral;
    it._loadControl(BSONElement());  // previous doesn't matter when we load literals

    // Traverse until we reach desired index or end
    auto e = end();
    for (size_t i = _indexLastLiteral; it != e && i < index; ++it, ++i) {
    }

    // Return EOO if not found
    if (it == e)
        return BSONElement();

    return *it;
}

}  // namespace mongo
