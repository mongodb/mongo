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
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension {

// Template helper to map our C API Type to a Cpp API implementation.
template <typename T>
struct c_api_to_cpp_api {
    using CppApi_t = T;  // default case.
};

template <typename T, bool>
class Handle;

/**
 * VTableAPI is a wrapper around a pointer that implements a C API via a vtable pointer.
 * VTableAPI abstracts access to the C API instance pointer, as well as its vtable.
 * It is important to note that VTableAPI does not take care of any ownership semantics of a
 * pointer. Any pointer provided to the VTableAPI constructor must be guaranteed to be kept
 * consistent for the entire lifetime of the VTableAPI object.
 *
 * In order to safely access a C API with a vtable, all such C API pointers must be accessed via a
 * VTableAPI implementation. Implementers must first create a template specialization for their C
 * API type (i.e VTableAPI<MyCApiType>), and implement the c_api_to_cpp_api type trait, which maps
 * the C API type to the VTableAPI implementation.
 *
 * When a VTableAPI object is instantiated, the static method assertVTableConstraints() is called.
 * Implementors of the VTableAPI specialization must implement this static method correctly,
 * ensuring all the vtable entries they expect to be present are valid.
 */
template <typename T, typename API = c_api_to_cpp_api<T>::CppApi_t>
class VTableAPI {
public:
    using VTablePtr_t = decltype(std::declval<T>().vtable);
    using VTable_t = typename std::remove_pointer<VTablePtr_t>::type;

    VTableAPI(T* ptr) : _ptr(ptr) {
        _assertValidVTable();
    }
    ~VTableAPI() = default;

    VTableAPI(const VTableAPI& other) : _ptr(other._ptr) {
        _assertValidVTable();
    }

    VTableAPI& operator=(const VTableAPI& other) {
        if (this != &other) {
            _ptr = other._ptr;
            _assertValidVTable();
        }
        return *this;
    }

    VTableAPI(VTableAPI&& other) : _ptr(std::exchange(other._ptr, nullptr)) {
        _assertValidVTable();
    }

    VTableAPI& operator=(VTableAPI&& other) {
        _ptr = std::exchange(other._ptr, nullptr);
        _assertValidVTable();
        return *this;
    }

    const T* get() const {
        return _ptr;
    }

    T* get() {
        return _ptr;
    }

    // TODO SERVER-115914: Make vtable protected. Update all tests that use vtable() to reflect
    // this.
    const VTable_t& vtable() const {
        assertValid();
        tassert(10596404, "Invalid vtable ptr!", _ptr->vtable != nullptr);
        return *(_ptr->vtable);
    }

    // Specializations of this class must implement this function to confirm the vtable is valid on
    // the API.
    static void assertVTableConstraints(const VTable_t& vtable) = delete;

    void assertValid() const {
        tassert(10596403, "Invalid VTable API", _ptr != nullptr);
    }

protected:
    void _assertValidVTable() const {
        if (_ptr) {
            API::assertVTableConstraints(vtable());
        }
    }

private:
    T* _ptr;
};

/**
 * Handle is a wrapper around a pointer to an extension C API type, allocated on the other side of
 * the API boundary, whose ownership can be transferred across the API boundary.
 * Whether or not Handle assumes ownership of the pointer is determined by the IsOwned template
 * parameter.
 *
 * When IsOwned is true, the pointer is owned by a unique_ptr and automatically deallocates the
 * associated memory when the Handle goes out of scope.
 * When IsOwned is false, Handle does not perform any deallocation when it goes out of scope.
 *
 * The C API type must have a vtable member. Under the hood, Handle holds a VTableAPI
 * member which is responsible for abstracting the vtable access for the pointer's C API.
 * Implementers are responsible for providing a template specialization for the VTableAPI according
 * to the C API's requirements.
 *
 * Handle does not expose the underlying VTableAPI, with the exception of via the -> operator. This
 * was done on purpose, as exposing the underlying VTableAPI as a reference which could be bound to
 * an l-value might open us up to situations in which the Handle's VTableAPI is accidentally mutated
 * via a copy or move constructor, leaving the original owning Handle with an invalid VTableAPI.
 *
 * Very important note on const-correctness: When we determine the VTableAPI type, we remove the
 * constness of the original template parameter. This is done so that we can use a single VTableAPI
 * implementation for each C API type, otherwise we would need a specialization for const and
 * non-const variants. While our actual VTableAPI member does not respect the logical constness,
 * we re-apply the originally intended logical constness on the VTableAPI pointer we return via the
 * the -> operators.
 *
 */
template <typename T, bool IsOwned>
class Handle final {
public:
    struct ExtensionObjectDeleter {
        template <typename E>
        void operator()(E* extensionObj) {
            extensionObj->vtable->destroy(extensionObj);
        }
    };

    using ExtensionObjectOwnedPtr = std::unique_ptr<T, ExtensionObjectDeleter>;

    using UnderlyingPtr_t = typename std::conditional_t<IsOwned, ExtensionObjectOwnedPtr, T*>;

    // Extract the CppApi type without constness so we can use a single VTableAPI
    // specialization for each C API type regardless of constness of the Handle param.
    using CppApi_t = c_api_to_cpp_api<std::remove_const_t<T>>::CppApi_t;

    // Ensure any instantiation of Handle has a properly defined mapping.
    static_assert(!std::is_same_v<T, CppApi_t>);

    Handle(T* ptr) : _ptr(ptr), _api(const_cast<std::remove_const_t<T>*>(ptr)) {
        if constexpr (IsOwned) {
            _assertValidDestroy();
        }
    }

    ~Handle() {}

    // Move only semantics if IsOwned = true.
    // Conditionally enable the copy constructor/assignment using a concept.
    Handle(const Handle& other)
    requires std::copy_constructible<UnderlyingPtr_t>
        : _ptr(other._ptr), _api(other._api) {
        if constexpr (IsOwned) {
            _assertValidDestroy();
        }
    }

    Handle& operator=(const Handle& other)
    requires std::copy_constructible<UnderlyingPtr_t>
    {
        _ptr = other._ptr;
        _api = other._api;
        if constexpr (IsOwned) {
            _assertValidDestroy();
        }
        return *this;
    }

    Handle(Handle&& other) : _ptr(std::move(other._ptr)), _api(std::move(other._api)) {
        if constexpr (IsOwned) {
            _assertValidDestroy();
        } else {
            other._ptr = nullptr;
        }
    }

    Handle& operator=(Handle&& other) {
        _ptr = std::move(other._ptr);
        _api = std::move(other._api);
        if constexpr (IsOwned) {
            _assertValidDestroy();
        } else {
            other._ptr = nullptr;
        }
        return *this;
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
        _api = CppApi_t(nullptr);
        if constexpr (std::is_same_v<UnderlyingPtr_t, ExtensionObjectOwnedPtr>) {
            return _ptr.release();
        } else {
            return std::exchange(_ptr, nullptr);
        }
    }

    /**
     * When exposing the underlying VTableAPI to invocations on the handle, return the logically
     * const version of the C API.
     * For example consider the following example:
     * C API: FooC
     * CPP API: FooVTable
     * If we instantiate the template with Handle<const FooC>, the intention to create a
     * logically const FooC. To simplify our template implementation such that we can use a single
     * VTableAPI specialization for both const/non-const, our VTableAPI member will be
     * non-const, but we only expose the logically const version of the VTableAPI. This way, callers
     * on the VTableAPI are bound to the originally intended logical-constness.
     */
    using LogicalConstCppApi_t = std::conditional_t<std::is_const_v<T>, const CppApi_t, CppApi_t>;
    LogicalConstCppApi_t* operator->() {
        return &_api;
    }

    const LogicalConstCppApi_t* operator->() const {
        return &_api;
    }

private:
    void _assertValidDestroy() {
        if (_ptr) {
            tassert(11511000, "OwnedHandle's vtable is null", _ptr->vtable != nullptr);
            tassert(10930100,
                    "OwnedHandle's vtable 'destroy' is null",
                    _ptr->vtable->destroy != nullptr);
        }
    }

    UnderlyingPtr_t _ptr;
    CppApi_t _api;
};

/**
 * OwnedHandle is a move-only wrapper around a raw pointer allocated by either the host or the
 * extension, whose ownership has been transferred to the other side. OwnedHandle acts as a wrapper
 * that abstracts the vtable and underlying pointer, and makes sure to destroy the associated
 * pointer when it goes out of scope.
 *
 * We use 'new' for allocating owned handles in the heap instead of the 'make_unique().release()'
 * pattern since we're not taking advantage of unique pointers but rather we rely on the scope for
 * destroying the pointer.
 */
template <typename T>
using OwnedHandle = Handle<T, true>;

/**
 * UnownedHandle is a wrapper around a raw pointer allocated by the either the host or the
 * extension, whose ownership has not been transferred to the other side. UnownedHandle acts as a
 * wrapper that abstracts the vtable and underlying pointer, but does not destroy the pointer when
 * it goes out of scope.
 *
 * We should NOT use 'new' for allocating unowned handles since keeping the unique pointer from
 * 'make_unique()' makes it more explicit that whichever side allocates the handle needs to also
 * take care of destroying the underlying pointer.
 */
template <typename T>
using UnownedHandle = Handle<T, false>;
}  // namespace mongo::extension
