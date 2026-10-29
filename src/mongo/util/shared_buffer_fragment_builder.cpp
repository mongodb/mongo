// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
