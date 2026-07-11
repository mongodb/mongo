// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/byte_buf.h"

#include "mongo/util/assert_util.h"

namespace mongo::extension {

ByteBuf::ByteBuf() : ::MongoExtensionByteBuf{&VTABLE} {}

ByteBuf::ByteBuf(const uint8_t* data, size_t len) : ::MongoExtensionByteBuf{&VTABLE} {
    assign(data, len);
}

ByteBuf::ByteBuf(const BSONObj& obj) : ::MongoExtensionByteBuf{&VTABLE} {
    assign(reinterpret_cast<const uint8_t*>(obj.objdata()), static_cast<size_t>(obj.objsize()));
}

void ByteBuf::assign(const uint8_t* data, size_t len) {
    if (len == 0) {
        _buffer.clear();
        return;
    }
    tassert(ErrorCodes::ExtensionSerializationError,
            "Data pointer cannot be null when length is non-zero",
            data != nullptr);
    _buffer.assign(data, data + len);
}

void ByteBuf::_extDestroy(::MongoExtensionByteBuf* buf) noexcept {
    delete static_cast<ByteBuf*>(buf);
}

MongoExtensionByteView ByteBuf::_extGetView(const ::MongoExtensionByteBuf* byteBufPtr) noexcept {
    const auto* byteBuf = static_cast<const ByteBuf*>(byteBufPtr);
    const auto vecSize = byteBuf->_buffer.size();
    const auto* data =
        (vecSize == 0) ? nullptr : reinterpret_cast<const uint8_t*>(byteBuf->_buffer.data());
    return MongoExtensionByteView{data, byteBuf->_buffer.size()};
}

const ::MongoExtensionByteBufVTable ByteBuf::VTABLE = {
    .destroy = &ByteBuf::_extDestroy,
    .get_view = &ByteBuf::_extGetView,
};

}  // namespace mongo::extension
