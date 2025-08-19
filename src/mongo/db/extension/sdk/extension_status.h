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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/extension/sdk/handle.h"
#include "mongo/stdx/mutex.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {

/**
 * ExtensionStatus is an abstraction for extensions to provide the status of API calls made
 * into the extension by the host.
 */
class ExtensionStatus final : public ::MongoExtensionStatus {
public:
    explicit ExtensionStatus() : ExtensionStatus(MONGO_EXTENSION_STATUS_OK) {}

    ExtensionStatus(int32_t code, const std::string& reason = std::string(""))
        : ::MongoExtensionStatus{&VTABLE}, _code(code), _reason(reason) {}

    ExtensionStatus(const std::exception& e, int32_t code) : ::MongoExtensionStatus{&VTABLE} {
        setException(e, code);
    }

    ~ExtensionStatus() = default;

    /**
     * Return a utf-8 string associated with `error`. May be empty.
     */
    std::string_view getReason() const {
        return _reason;
    }

    int32_t getCode() const {
        return _code;
    }

    /**
     * Populate the code and reason using an exception. Does not store or propagate the exception.
     */
    void setException(const std::exception& e, int32_t code) {
        _code = code;
        _reason = e.what();
    }

private:
    static const ::MongoExtensionStatusVTable VTABLE;

    static void _extDestroy(::MongoExtensionStatus* status) noexcept {
        delete static_cast<mongo::extension::sdk::ExtensionStatus*>(status);
    }

    static int32_t _extGetCode(const ::MongoExtensionStatus* status) noexcept {
        return static_cast<const mongo::extension::sdk::ExtensionStatus*>(status)->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(
            static_cast<const mongo::extension::sdk::ExtensionStatus*>(status)->getReason());
    }
    /**
     * _code may be MONGO_EXTENSION_STATUS_RUNTIME_ERROR if there was no error code associated
     * with the exception.
     */
    int32_t _code;
    std::string _reason;
};

/**
 * ExtensionStatusOK is an ExtensionStatus that is always OK.
 */
class ExtensionStatusOK final : public ::MongoExtensionStatus {
public:
    ~ExtensionStatusOK() {
        sInstanceCount--;
    }

    std::string_view getReason() const {
        return std::string_view();
    }

    int32_t getCode() const {
        return MONGO_EXTENSION_STATUS_OK;
    }

    // Provide access to our singleton instance.
    static ExtensionStatusOK& getInstance() {
        static ExtensionStatusOK singleton;
        return singleton;
    }

    static size_t getInstanceCount();

private:
    ExtensionStatusOK() : ::MongoExtensionStatus{&VTABLE} {
        sInstanceCount++;
    }

    static void _extDestroy(::MongoExtensionStatus* /*status*/) noexcept {
        /* NoOp! We would like to have a singleton ExtensionStatusOK that can be passed across the
         * boundary to avoid having to allocate a status every time we make an API call. However,
         * callers assume they are taking ownership of the returned ExtensionStatus pointer, so they
         * will call destroy once they no longer need the status. By making destroy a noOp, we avoid
         * having the destructor of our instance be called.
         */
    }
    static int32_t _extGetCode(const ::MongoExtensionStatus* status) noexcept {
        return static_cast<const mongo::extension::sdk::ExtensionStatusOK*>(status)->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(
            static_cast<const mongo::extension::sdk::ExtensionStatusOK*>(status)->getReason());
    }

    static const ::MongoExtensionStatusVTable VTABLE;
    static size_t sInstanceCount;
};

/**
 * ExtensionStatus is an abstraction for extensions to provide the status of API calls made into
 * the extension by the host.
 */
class ExtensionStatusException final : public ::MongoExtensionStatus {
public:
    ExtensionStatusException(std::exception_ptr&& e,
                             int32_t code,
                             const std::string& reason = std::string(""))
        : ::MongoExtensionStatus{&VTABLE}, _code(code), _reason(reason), _exception(std::move(e)) {}

    ~ExtensionStatusException() = default;

    /**
     * Return a utf-8 string associated with `error`. May be empty.
     */
    std::string_view getReason() const {
        return _reason;
    }

    int32_t getCode() const {
        return _code;
    }

    static std::exception_ptr extractException(::MongoExtensionStatus& status) {
        if (status.vtable == &VTABLE) {
            return std::move(static_cast<ExtensionStatusException*>(&status)->_exception);
        } else {
            return nullptr;
        }
    }

private:
    static void _extDestroy(::MongoExtensionStatus* status) noexcept {
        delete static_cast<mongo::extension::sdk::ExtensionStatusException*>(status);
    }

    static int32_t _extGetCode(const ::MongoExtensionStatus* status) noexcept {
        return static_cast<const mongo::extension::sdk::ExtensionStatusException*>(status)
            ->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(
            static_cast<const mongo::extension::sdk::ExtensionStatusException*>(status)
                ->getReason());
    }

    static const ::MongoExtensionStatusVTable VTABLE;

    /**
     * _code may be MONGO_EXTENSION_STATUS_RUNTIME_ERROR if there was no error code associated with
     *  the exception.
     */
    int32_t _code;
    std::string _reason;
    std::exception_ptr _exception;
};
/**
 * HostStatusHandle is an owned handle wrapper around a MongoExtensionStatus.
 * Typically this is a handle around a MongoExtensionStatus allocated by the host whose ownership
 * has been transferred to the extension.
 */
class HostStatusHandle : public OwnedHandle<::MongoExtensionStatus> {
public:
    HostStatusHandle(::MongoExtensionStatus* status) : OwnedHandle<::MongoExtensionStatus>(status) {
        _assertValidVTable();
    }

    /**
     * Return a non-zero code associated with `error`.
     */
    int getCode() const {
        assertValid();
        return vtable().get_code(get());
    }

    /**
     * Return a utf-8 string associated with `MongoExtensionStatus`. May be empty.
     */
    std::string_view getReason() const {
        assertValid();
        return byteViewAsStringView(vtable().get_reason(get()));
    }

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(10930105, "HostStatus 'get_code' is null", vtable.get_code != nullptr);
        tassert(10930106, "HostStatus 'get_reason' is null", vtable.get_reason != nullptr);
    };
};

/**
 * Encompasses a class of exceptions due to lack of resources or conflicting resources. Can be used
 * to conveniently catch all derived exceptions instead of enumerating each of them individually.
 */
class ExtensionDBException final : public DBException {
public:
    using DBException::DBException;
    ExtensionDBException(HostStatusHandle extensionStatus)
        : DBException(error_details::makeStatus(extensionStatus.getCode(),
                                                std::string(extensionStatus.getReason()))),
          _extensionStatus(std::move(extensionStatus)) {}

    HostStatusHandle extractStatus() {
        stdx::unique_lock lk(_mutex);
        return std::move(_extensionStatus);
    }

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {};
    stdx::mutex _mutex;
    HostStatusHandle _extensionStatus;
};

/**
 * enterCXX is a template helper that allows functions called at the C API boundary to call into C++
 * code safely. Exceptions are not allowed to cross the API boundary. Executing C++ wrapped in this
 * enterCXX function, translates any C++ exceptions into a MongoExtensionStatus* that is compatible
 * with the C API and can be passed across to the caller.
 */
template <typename Fn>
::MongoExtensionStatus* enterCXX(Fn&& fn) {
    try {
        fn();
        return &ExtensionStatusOK::getInstance();
    } catch (ExtensionDBException& e) {
        /**
         * If we caught an ExtensionDBException, we can propagate the underlying extension status.
         * Note that we catch here by non-const reference, because we must mutate the exception
         * object to extract the underlying extension status.
         */
        return e.extractStatus().release();
    } catch (const DBException& e) {
        /**
         * If we caught any other type of exception, we capture the current C++ exception in our
         * custom ExtensionStatusException type. If we ever re-enter into our C++ execution context,
         * we can rethrow the captured exception.
         */
        return std::make_unique<ExtensionStatusException>(std::current_exception(), e.code())
            .release();
    } catch (...) {
        return std::make_unique<ExtensionStatusException>(std::current_exception(),
                                                          MONGO_EXTENSION_STATUS_RUNTIME_ERROR)
            .release();
    }
}

void enterC_ErrorHandler(HostStatusHandle status);

/**
 * enterC is a template helper that allows functions called at the C API boundary on the C++ side to
 * call into C from C++. The provided functor must return a MongoExtensionStatus*, which this helper
 * will translate into a C++ exception.
 */
template <typename Fn>
void enterC(Fn&& fn) {
    HostStatusHandle status(fn());
    if (auto code = status.getCode(); MONGO_unlikely(code != MONGO_EXTENSION_STATUS_OK)) {
        return enterC_ErrorHandler(std::move(status));
    }
}
}  // namespace mongo::extension::sdk
