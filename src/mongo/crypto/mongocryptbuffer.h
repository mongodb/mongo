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

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/crypto/mongocrypt_definitions.h"

#include <memory>

typedef struct __mongocrypt_buffer_t _mongocrypt_buffer_t;

namespace mongo {

/**
 * C++ friendly wrapper around libmongocrypt's private _mongocrypt_buffer_t and its associated
 * functions.
 *
 * This class may or may not own data.
 */
class MongoCryptBuffer {
public:
    // Note that the copy constructor is deleted to force
    // callers to make an explicit decision about borrowing versus copying.
    MongoCryptBuffer();
    MongoCryptBuffer(const MongoCryptBuffer&) = delete;
    MongoCryptBuffer(MongoCryptBuffer&&) = default;
    ~MongoCryptBuffer();

    MongoCryptBuffer& operator=(const MongoCryptBuffer&) = delete;
    MongoCryptBuffer& operator=(MongoCryptBuffer&&) = default;

    /**
     * Copy a ConstDataRange into a _mongocrypt_buffer_t.
     * New space will be allocated.
     *
     * Note consider the borrow() factory if the MongoCryptBuffer object
     * need not outlive the original _mongocrypt_buffer_t.
     */
    static MongoCryptBuffer copy(ConstDataRange src);

    /**
     * Take ownership of an already allocated _mongocrypt_buffer_t.
     * buffer->owned MUST be true, or an exception will be thrown.
     * Note that {src->owned} will be reset to prevent accidental double frees.
     */
    static MongoCryptBuffer adopt(_mongocrypt_buffer_t* src);

    /**
     * Hold a reference to the data in buffer.
     * The original buffer MUST remain alive while using this object.
     */
    static MongoCryptBuffer borrow(ConstDataRange src);

    /**
     * Hold a reference to the data in buffer.
     * The original buffer MUST remain alive while using this object.
     */
    static MongoCryptBuffer borrow(const _mongocrypt_buffer_t* src);

    /**
     * Indicates if the currently held _mongocrypt_buffer_t is "owned";
     * That is, if the storage need be allocated on destruction.
     */
    bool isOwned() const;

    /**
     * Make a new, owned copy of the underlying buffer.
     */
    MongoCryptBuffer duplicate() const;

    /**
     * Copy the MongoCryptBuffer object itself, but merely borrow the underlying
     * _mongocrypt_buffer_t.
     */
    MongoCryptBuffer borrow() const;

    /**
     * Resize the storage allocated to the buffer.
     * If the buffer is not currently owned, a copy will be made.
     */
    MongoCryptBuffer& resize(std::size_t);

    /**
     * Dereference the internal object for passing to mongocrypt APIs.
     */
    const _mongocrypt_buffer_t* get() const {
        return _buffer.get();
    }

    /**
     * Dereference the internal object for passing to mongocrypt APIs.
     */
    _mongocrypt_buffer_t* get() {
        return _buffer.get();
    }

    /**
     * The length of the data in the underlying buffer.
     */
    uint32_t size() const;

    /**
     * The raw data pointer (const).
     */
    const uint8_t* data() const;

    /**
     * The raw data pointer.
     */
    uint8_t* data();

    /**
     * Returns TRUE if the underlying buffer is empty.
     */
    bool empty() const {
        return size() == 0;
    }

    /**
     * Convert to a ConstDataRange.
     */
    ConstDataRange toCDR() const {
        return {data(), size()};
    }

private:
    std::unique_ptr<_mongocrypt_buffer_t> _buffer;
};

}  // namespace mongo
