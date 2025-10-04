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

#include "mongo/util/shared_buffer_fragment.h"

namespace mongo {

void SharedBufferFragmentBuilder::freeUnused() {
    if (_activeBuffers.empty()) {
        return;
    }

    // Normally all buffers are expected to no longer be shared and can be freed immediately,
    // however, the last buffer may still be shared with the owning SharedBufferFragmentBuilder.
    auto it = std::remove_if(
        _activeBuffers.begin(), _activeBuffers.end(), [](auto&& buf) { return !buf.isShared(); });
    _activeBuffers.erase(it, _activeBuffers.end());

    // Recalculate memory used by active buffers.
    size_t remaining = _buffer.capacity();
    for (auto&& buf : _activeBuffers) {
        remaining += buf.capacity();
    }
    _memUsage = remaining;
}

SharedBuffer SharedBufferFragmentBuilder::_realloc(SharedBuffer&& existing,
                                                   size_t offset,
                                                   size_t existingSize,
                                                   size_t newSize) {
    // If nothing else is using the internal buffer it would be safe to use realloc. But as
    // this potentially is a large buffer realloc would need copy all of it as it doesn't
    // know how much is actually used. So we create a new buffer in all cases
    auto newBuffer = SharedBuffer::allocate(newSize);
    _memUsage += newBuffer.capacity();

    // When existingSize is 0 we may be in an initial alloc().
    if (existing && existingSize) {
        memcpy(newBuffer.get(), existing.get() + offset, existingSize);
    }

    // If this buffer is actively used somewhere, we'll need to keep a reference to it for
    // tracking memory usage since there may be other fragments that are also holding onto a
    // reference. Otherwise, we let it get freed. Callers will have to take care to clean up
    // these shared references regularly using freeUnused().
    if (existing.isShared()) {
        _activeBuffers.push_back(std::move(existing));
    } else {
        _memUsage -= existing.capacity();
    }
    return newBuffer;
}
}  // namespace mongo
