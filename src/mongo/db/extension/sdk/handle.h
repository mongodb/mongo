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
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo::extension::sdk {
/**
 * Handle is a wrapper around a pointer allocated by the host, whose ownership
 * can be transferred to the extension. Whether or not Handle assumes ownership of the
 * pointer is determined by the IsOwned template parameter. When IsOwned is false, Handle acts as a
 * wrapper that abstracts the vtable and underlying pointer, but does not destroy the pointer when
 * it goes out of scope. When IsOwned is true, the pointer is owned by a unique_ptr and
 * automatically deallocates the associated memory when the Handle goes out of scope. Note, that
 * when a Handle owns the underlying memory, it only supports move semantics. Special care must be
 * taken to ensure the correct policy is assumed based on the context where the handle is used.
 */
template <typename T, bool IsOwned>
class Handle {
public:
    struct ExtensionObjectDeleter {
        template <typename E>
        void operator()(E* extensionObj) {
            extensionObj->vtable->destroy(extensionObj);
        }
    };

    using ExtensionObjectOwnedPtr = std::unique_ptr<T, ExtensionObjectDeleter>;

    using UnderlyingPtr_t = typename std::conditional_t<IsOwned, ExtensionObjectOwnedPtr, T*>;

    using VTablePtr_t = decltype(std::declval<T>().vtable);
    using VTable_t = typename std::remove_pointer<VTablePtr_t>::type;

    virtual ~Handle() {}

    // Move only semantics if IsOwned = true.
    // Conditionally enable the copy constructor/assignment using a concept.
    Handle(const Handle& other)
    requires std::copy_constructible<UnderlyingPtr_t>
    {
        if (other.isValid()) {
            other._assertValidVTable();
        }
        _ptr = other._ptr;
    }

    Handle& operator=(const Handle& other)
    requires std::copy_constructible<UnderlyingPtr_t>
    {
        _ptr = other._ptr;
        if (_ptr) {
            _assertValidVTable();
        }
        return *this;
    }
    Handle(Handle&& other) {
        if (other.isValid()) {
            other._assertValidVTable();
        }
        _ptr = std::move(other._ptr);
    }

    Handle& operator=(Handle&& other) {
        _ptr = std::move(other._ptr);
        if (_ptr) {
            _assertValidVTable();
        }
        return *this;
    }

    void assertValid() const {
        tassert(10596403, "Invalid handle", isValid());
    }

    bool isValid() const {
        return get() != nullptr;
    }

    T* get() const {
        if constexpr (std::is_same_v<UnderlyingPtr_t, ExtensionObjectOwnedPtr>) {
            return _ptr.get();
        } else {
            return _ptr;
        }
    }

    T* release() {
        if constexpr (std::is_same_v<UnderlyingPtr_t, ExtensionObjectOwnedPtr>) {
            return _ptr.release();
        } else {
            auto ptr = _ptr;
            _ptr = nullptr;
            return ptr;
        }
    }

    // NOTE: This method should only be called when isValid() is true.
    const VTable_t& vtable() const {
        tassert(10596404, "Invalid vtable ptr!", _ptr->vtable != nullptr);
        return *_ptr->vtable;
    }

protected:
    explicit Handle(T* ptr) : _ptr(ptr) {}

    // Derived classes must implement this function to confirm the vtable does not contain nullptrs.
    virtual void _assertVTableConstraints(const VTable_t& vtable) const = 0;

    // NOTE: Derivations of Handle MUST call _assertValidVTable() from the constructor.
    void _assertValidVTable() const {
        if (isValid()) {
            const auto& vtbl = vtable();
            if constexpr (IsOwned) {
                tassert(10930100, "OwnedHandle's 'destroy' is null", vtbl.destroy != nullptr);
            }
            _assertVTableConstraints(vtbl);
        }
    }

private:
    UnderlyingPtr_t _ptr;
};

/**
 * OwnedHandle is a move-only wrapper around a raw pointer allocated by the host, whose
 * ownership has been transferred to the host. OwnedHandle acts as a wrapper that
 * abstracts the vtable and underlying pointer, and makes sure to destroy the associated pointer
 * when it goes out of scope.
 */
template <typename T>
using OwnedHandle = Handle<T, true>;

/**
 * UnownedHandle is a wrapper around a raw pointer allocated by the host, whose
 * ownership has not been transferred to the extension. UnownedHandle acts as a wrapper that
 * abstracts the vtable and underlying pointer, but does not destroy the pointer when it goes out of
 * scope.
 */
template <typename T>
using UnownedHandle = Handle<T, false>;
}  // namespace mongo::extension::sdk
