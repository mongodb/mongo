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

#include "mongo/bson/util/bsoncolumnbuilder.h"

#include "mongo/bson/util/simple8b_type_util.h"

#include <memory>

namespace mongo {
static constexpr uint8_t kMaxCount = 16;
static constexpr uint8_t kCountMask = 0x0F;
static constexpr uint8_t kNoScaleControl = 0x80;

BSONColumnBuilder::BSONColumnBuilder(StringData fieldName)
    : _simple8bBuilder(_createSimple8bBuilder()), _fieldName(fieldName) {

    // Store EOO type with empty field name as previous.
    _storePrevious(BSONElement());
}

BSONElement BSONColumnBuilder::_previous() const {
    return {_prev.get(), 1, _prevSize, BSONElement::CachedSizeTag{}};
}

BSONColumnBuilder& BSONColumnBuilder::append(BSONElement elem) {
    auto type = elem.type();
    auto previous = _previous();

    // If we detect a type change (or this is first value). Flush all pending values in Simple-8b
    // and write uncompressed literal.
    if (previous.type() != elem.type()) {
        _storePrevious(elem);
        _simple8bBuilder.flush();
        _writeLiteralFromPrevious();
        return *this;
    }

    // Store delta in Simple-8b if types match
    int64_t delta = 0;
    if (!elem.binaryEqualValues(previous)) {
        switch (type) {
            case NumberInt:
                delta = elem._numberInt() - previous._numberInt();
                break;
            case NumberLong:
                delta = elem._numberLong() - previous._numberLong();
                break;
            default:
                // Nothing else is implemented yet
                invariant(false);
        };
    }

    bool result = _simple8bBuilder.append(Simple8bTypeUtil::encodeInt64(delta));
    _storePrevious(elem);

    // Store uncompressed literal if value is outside of range of encodable values.
    if (!result) {
        _simple8bBuilder.flush();
        _writeLiteralFromPrevious();
    }

    return *this;
}

BSONColumnBuilder& BSONColumnBuilder::skip() {
    _simple8bBuilder.skip();
    return *this;
}

BSONBinData BSONColumnBuilder::finalize() {
    _simple8bBuilder.flush();

    // Write EOO at the end
    _bufBuilder.appendChar(EOO);

    return {_bufBuilder.buf(), _bufBuilder.len(), BinDataType::Column};
}

void BSONColumnBuilder::_storePrevious(BSONElement elem) {
    auto valuesize = elem.valuesize();

    // Add space for type byte and field name null terminator
    auto size = valuesize + 2;

    // Re-allocate buffer if not large enough
    if (size > _prevCapacity) {
        _prevCapacity = size;
        _prev = std::make_unique<char[]>(_prevCapacity);

        // Store null terminator, this byte will never change
        _prev[1] = '\0';
    }

    // Copy element into buffer for previous. Omit field name.
    _prev[0] = elem.type();
    memcpy(_prev.get() + 2, elem.value(), valuesize);
    _prevSize = size;
}

void BSONColumnBuilder::_writeLiteralFromPrevious() {
    // Write literal without field name and reset control byte to force new one to be written when
    // appending next value
    _bufBuilder.appendBuf(_prev.get(), _prevSize);
    _controlByteOffset = 0;
}

void BSONColumnBuilder::_incrementSimple8bCount() {
    char* byte;
    uint8_t count;

    if (_controlByteOffset == 0) {
        // Allocate new control byte if we don't already have one. Record its offset so we can find
        // it even if the underlying buffer reallocates.
        byte = _bufBuilder.skip(1);
        _controlByteOffset = std::distance(_bufBuilder.buf(), byte);
        count = 0;
    } else {
        // Read current count from previous control byte
        byte = _bufBuilder.buf() + _controlByteOffset;
        count = (*byte & kCountMask) + 1;
    }

    // Write back new count and clear offset if we have reached max count
    *byte = kNoScaleControl | (count & kCountMask);
    if (count + 1 == kMaxCount) {
        _controlByteOffset = 0;
    }
}

Simple8bBuilder<uint64_t> BSONColumnBuilder::_createSimple8bBuilder() {
    return Simple8bBuilder<uint64_t>([this](uint64_t block) {
        // Write/update block count
        _incrementSimple8bCount();

        // Write Simple-8b block
        _bufBuilder.appendNum(block);
        return true;
    });
}

}  // namespace mongo
