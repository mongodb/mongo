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

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/crypto/symmetric_crypto.h"


namespace mongo {
namespace crypto {


/** Accumulates unaligned input, and presents it in block aligned segments to a caller provided
 * callback.
 */
class BlockPacker {
public:
    BlockPacker()
        : _blockData(_blockBuffer->data(), _blockBuffer->size()), _blockCursor(_blockData) {}

    template <typename OperationFn>
    StatusWith<size_t> pack(ConstDataRange inData, OperationFn&& op) {
        ConstDataRangeCursor inCursor(inData);

        // We must always attempt to accumulate a block. We must only perform the operation when we
        // would overflow a block. The finalization operation must have the last block of data
        // available.

        size_t bytesWritten = 0;

        // Pad block. This needs to happen if the block is partially full from a previous iteration.
        // Otherwise, we don't need to align with earlier input, and can consume the data straight
        // out of the input.
        if (_blockCursor.length() < aesBlockSize) {
            size_t bytesToFill = std::min(inCursor.length(), _blockCursor.length());
            ConstDataRange bytesToFillRange(inCursor.data(), bytesToFill);
            inCursor.advance(bytesToFill);

            _blockCursor.writeAndAdvance(bytesToFillRange);
        }

        // If we've consumed all the input, we're done. Processing will happen in later method
        // calls.
        if (inCursor.length() == 0) {
            return 0;
        }

        // We have more input to consume. If we already have a block, flush it.
        if (_blockCursor.length() == 0) {
            StatusWith<size_t> swBlockBytesWritten = op(ConstDataRange(*_blockBuffer));

            if (!swBlockBytesWritten.isOK()) {
                return swBlockBytesWritten.getStatus();
            }

            bytesWritten += swBlockBytesWritten.getValue();
            _blockCursor = DataRangeCursor(_blockData);
        }

        // We can now process a whole number of blocks. However, we must not process the last block,
        // which is defered until later calls.
        std::lldiv_t results = std::div(static_cast<const long long>(inCursor.length()),
                                        static_cast<const long long>(aesBlockSize));
        size_t blocksToProcess = results.quot;
        size_t bytesRemaining = results.rem;
        if (blocksToProcess > 0 && bytesRemaining == 0) {
            blocksToProcess -= 1;
        }
        size_t bytesToProcess = blocksToProcess * aesBlockSize;

        if (bytesToProcess) {
            ConstDataRange invocationInput(inCursor.data(), inCursor.data() + bytesToProcess);
            inCursor.advance(bytesToProcess);

            StatusWith<size_t> swBytesDecrypted = op(invocationInput);

            if (!swBytesDecrypted.isOK()) {
                return swBytesDecrypted.getStatus();
            }

            bytesWritten += swBytesDecrypted.getValue();
        }

        // We must now pack the remaining data into the block
        invariant(_blockCursor.length() >= inCursor.length());
        _blockCursor.writeAndAdvance(inCursor);

        return static_cast<size_t>(bytesWritten);
    }

    ConstDataRange getBlock() const {
        return ConstDataRange(_blockData.data(), _blockData.length() - _blockCursor.length());
    }


private:
    // buffer to store a single block of data, to be encrypted by update when filled, or by finalize
    // with padding. 16 is the block length for AES.
    SecureArray<uint8_t, aesBlockSize> _blockBuffer;
    DataRange _blockData;
    DataRangeCursor _blockCursor;
};

}  // namespace crypto
}  // namespace mongo
