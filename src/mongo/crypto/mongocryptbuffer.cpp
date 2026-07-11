// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
