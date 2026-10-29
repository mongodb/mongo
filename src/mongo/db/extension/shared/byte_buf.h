// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace mongo::extension {

/**
 * C++ implementation of MongoExtensionByteBuf for use when crossing the API boundary. Owns a buffer
 * and exposes it via the C API vtable so the extension or host can pass byte data without owning
 * the underlying memory. Used by both host and SDK.
 */
class ByteBuf final : public ::MongoExtensionByteBuf {
public:
    ByteBuf();
    ByteBuf(const uint8_t* data, size_t len);
    /**
     * Constructs a ByteBuf from a BSONObj. The BSONObj must be safe to copy (it must own its
     * data or the data must outlive this ByteBuf).
     */
    ByteBuf(const BSONObj& obj);

    /**
     * Replace contents with [data, data+len). If len==0, clears the buffer.
     * Precondition: data must be non-null when len > 0.
     */
    void assign(const uint8_t* data, size_t len);

    static ::MongoExtensionByteBufVTable getVTable() {
        return VTABLE;
    }

private:
    static void _extDestroy(::MongoExtensionByteBuf* buf) noexcept;
    static MongoExtensionByteView _extGetView(const ::MongoExtensionByteBuf* byteBufPtr) noexcept;

    static const ::MongoExtensionByteBufVTable VTABLE;
    std::vector<uint8_t> _buffer;
};
}  // namespace mongo::extension
