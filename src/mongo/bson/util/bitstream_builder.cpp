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

#include "mongo/bson/util/bitstream_builder.h"

namespace mongo {
void BitStreamBuilder::appendBits(uint64_t data, int32_t numBits) {
    // If bitPos isn't 0, we need to append to the last char already in buffer
    uint8_t bitShift = 0;
    if (_bitPos != 0 && numBits > 0) {
        bitShift = 8 - _bitPos;
        // Modify current last byte in buffer to append first bits of new data
        _currentLastByte = _currentLastByte | ((data & 0xFF) << _bitPos);
        _buf.buf()[_buf.len() - 1] = _currentLastByte;
        numBits -= bitShift;
    }
    // Was able to store all bits, so we return
    if (numBits <= 0) {
        _bitPos = (_bitPos + numBits + bitShift) % 8;
        return;
    }
    int byteShiftCount = 0;
    // Keep adding until numBits < 8
    while (numBits > 8) {
        _buf.appendChar((data >> (8 * byteShiftCount + bitShift)) & 0xFF);
        numBits -= 8;
        byteShiftCount += 1;
    }
    // Add final bits by shifting to final byte, then slide for the bits that were added if our
    // previous byte was not completely full. We then and with a right shifted FF to avoid including
    // unwanted data from the previous byte
    _currentLastByte = (data >> (8 * byteShiftCount + bitShift)) & (0xFF >> (8 - numBits));
    _buf.appendChar(_currentLastByte);
    _bitPos = numBits % 8;
}

const BufBuilder* BitStreamBuilder::getBuffer() const {
    return &_buf;
}

uint8_t BitStreamBuilder::getCurrentBitPos() {
    return _bitPos;
}
}  // namespace mongo
