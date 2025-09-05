/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/extension/sdk/handle.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace mongo::extension::sdk {

class VecByteBuf final : public ::MongoExtensionByteBuf {
public:
    static const ::MongoExtensionByteBufVTable VTABLE;

    VecByteBuf();
    VecByteBuf(const uint8_t* data, size_t len);
    /**
     * Constructs a VecByteBuf from a BSONObj. The BSONObj must be safe to copy (it must own its
     * data or the data must outlive this VecByteBuf).
     */
    VecByteBuf(const BSONObj& obj);

    /**
     * Replace contents with [data, data+len). If len==0, clears the buffer.
     * Precondition: data must be non-null when len > 0.
     */
    void assign(const uint8_t* data, size_t len);

private:
    static void _extDestroy(::MongoExtensionByteBuf* buf) noexcept;
    static MongoExtensionByteView _extGetView(const ::MongoExtensionByteBuf* byteBufPtr) noexcept;

    std::vector<uint8_t> _buffer;
};

/**
 * VecByteBufHandle is an owned handle wrapper around a VecByteBuf.
 * Typically this is a handle around a VecByteBuf allocated by the host whose ownership
 * has been transferred to the extension.
 */
class VecByteBufHandle : public OwnedHandle<VecByteBuf> {
public:
    VecByteBufHandle(VecByteBuf* buf) : OwnedHandle<VecByteBuf>(buf) {
        _assertValidVTable();
    }

    /**
     * Get a read-only byte view of the contents of VecByteBuf.
     */
    MongoExtensionByteView getByteView() const {
        assertValid();
        return vtable().get_view(get());
    }

    /**
     * Get a read-only string view of the contents of VecByteBuf.
     */
    std::string_view getStringView() const {
        assertValid();
        return byteViewAsStringView(vtable().get_view(get()));
    }

    /**
     * Destroy VecByteBuf and free all associated resources.
     */
    void destroy() const {
        assertValid();
        vtable().destroy(get());
    }

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(10806301, "VecByteBuf 'get_view' is null", vtable.get_view != nullptr);
        tassert(10806302, "VecByteBuf 'destroy' is null", vtable.destroy != nullptr);
    };
};

}  // namespace mongo::extension::sdk
