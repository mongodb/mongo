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

#include "mongo/crypto/mongocryptbuffer.h"

extern "C" {
#include <mongocrypt-buffer-private.h>
}

namespace mongo {

MongoCryptBuffer::MongoCryptBuffer()
    : _buffer(std::unique_ptr<_mongocrypt_buffer_t>(new _mongocrypt_buffer_t)) {
    _mongocrypt_buffer_init(_buffer.get());
}

MongoCryptBuffer::~MongoCryptBuffer() {
    _mongocrypt_buffer_cleanup(_buffer.get());
}

MongoCryptBuffer MongoCryptBuffer::copy(ConstDataRange src) {
    MongoCryptBuffer ret;
    uassert(ErrorCodes::OperationFailed,
            "_mongocrypt_buffer_copy_from_data_and_size failed",
            _mongocrypt_buffer_copy_from_data_and_size(
                ret.get(), src.data<std::uint8_t>(), src.length()));
    return ret;
}

MongoCryptBuffer MongoCryptBuffer::adopt(_mongocrypt_buffer_t* src) {
    MongoCryptBuffer ret;
    *ret.get() = *src;
    // Mutate the donor buffer since we now own its data.
    src->owned = false;
    return ret;
}

MongoCryptBuffer MongoCryptBuffer::borrow(ConstDataRange src) {
    MongoCryptBuffer ret;
    auto* dest = ret.get();
    dest->data = const_cast<std::uint8_t*>(src.data<std::uint8_t>());
    dest->len = src.length();
    dest->owned = false;
    return ret;
}

MongoCryptBuffer MongoCryptBuffer::borrow(const _mongocrypt_buffer_t* src) {
    MongoCryptBuffer ret;
    auto* dest = ret.get();
    *dest = *const_cast<_mongocrypt_buffer_t*>(src);
    // We do not own our copy, it belongs to the donor.
    dest->owned = false;
    return ret;
}

bool MongoCryptBuffer::isOwned() const {
    return _buffer->owned;
}

MongoCryptBuffer MongoCryptBuffer::duplicate() const {
    MongoCryptBuffer ret;
    _mongocrypt_buffer_copy_to(_buffer.get(), ret.get());
    return ret;
}

MongoCryptBuffer MongoCryptBuffer::borrow() const {
    MongoCryptBuffer ret;
    auto* dest = ret.get();
    *dest = *_buffer.get();
    // We do not own our copy, it belongs to the donor.
    dest->owned = false;
    return ret;
}

MongoCryptBuffer& MongoCryptBuffer::resize(std::size_t sz) {
    _mongocrypt_buffer_resize(_buffer.get(), sz);
    // libmongocrypt will duplicate the underlying buffer if it is not already owned.
    invariant(isOwned());
    return *this;
}

uint32_t MongoCryptBuffer::size() const {
    return _buffer->len;
}

const uint8_t* MongoCryptBuffer::data() const {
    return _buffer->data;
}

uint8_t* MongoCryptBuffer::data() {
    return _buffer->data;
}

}  // namespace mongo
