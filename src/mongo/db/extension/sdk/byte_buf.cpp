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

#include "mongo/db/extension/sdk/byte_buf.h"

namespace mongo::extension::sdk {

VecByteBuf::VecByteBuf() : ::MongoExtensionByteBuf{&VTABLE} {}

VecByteBuf::VecByteBuf(const uint8_t* data, size_t len) : ::MongoExtensionByteBuf{&VTABLE} {
    assign(data, len);
}

VecByteBuf::VecByteBuf(const BSONObj& obj) : ::MongoExtensionByteBuf{&VTABLE} {
    assign(reinterpret_cast<const uint8_t*>(obj.objdata()), static_cast<size_t>(obj.objsize()));
}

void VecByteBuf::assign(const uint8_t* data, size_t len) {
    if (len == 0) {
        _buffer.clear();
        return;
    }
    tassert(10806300, "Data pointer cannot be null when length is non-zero", data != nullptr);
    _buffer.assign(data, data + len);
}

void VecByteBuf::_extDestroy(::MongoExtensionByteBuf* buf) noexcept {
    delete static_cast<VecByteBuf*>(buf);
}

MongoExtensionByteView VecByteBuf::_extGetView(const ::MongoExtensionByteBuf* byteBufPtr) noexcept {
    const auto* vecByteBuf = static_cast<const VecByteBuf*>(byteBufPtr);
    const auto vecSize = vecByteBuf->_buffer.size();
    const auto* data =
        (vecSize == 0) ? nullptr : reinterpret_cast<const uint8_t*>(vecByteBuf->_buffer.data());
    return MongoExtensionByteView{data, vecByteBuf->_buffer.size()};
}

const ::MongoExtensionByteBufVTable VecByteBuf::VTABLE = {&VecByteBuf::_extDestroy,
                                                          &VecByteBuf::_extGetView};

}  // namespace mongo::extension::sdk
