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
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension {

/**
 * ExtensionStatusOK is an implementation of ::MongoExtensionStatus that is always OK.
 *
 * We use a singleton instance to avoid having to allocate a status every time we need to return
 * success across the API boundary.
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

    static ::MongoExtensionStatusVTable getVTable() {
        return VTABLE;
    }

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
        return static_cast<const ExtensionStatusOK*>(status)->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(static_cast<const ExtensionStatusOK*>(status)->getReason());
    }

    static void _extSetCode(::MongoExtensionStatus* status, int32_t newCode) noexcept {
        // No-op for ExtensionStatusOK
    }

    static ::MongoExtensionStatus* _extSetReason(::MongoExtensionStatus* status,
                                                 MongoExtensionByteView newReason) noexcept;

    static MongoExtensionStatus* _extClone(const ::MongoExtensionStatus* status,
                                           ::MongoExtensionStatus** output) noexcept;

    static const ::MongoExtensionStatusVTable VTABLE;
    static size_t sInstanceCount;
};

/**
 * ExtensionGenericStatus is an implementation of ::MongoExtensionStatus that can be set across the
 * API boundary.
 */
class ExtensionGenericStatus final : public ::MongoExtensionStatus {
public:
    ExtensionGenericStatus()
        : ::MongoExtensionStatus{&VTABLE}, _code(MONGO_EXTENSION_STATUS_OK), _reason("") {}

    ExtensionGenericStatus(int32_t code, std::string reason)
        : ::MongoExtensionStatus{&VTABLE}, _code(code), _reason(reason) {}

    ~ExtensionGenericStatus() = default;

    std::string_view getReason() const {
        return _reason;
    }

    int32_t getCode() const {
        return _code;
    }

    void setCode(int32_t code) {
        _code = code;
    }

    void setReason(const std::string_view& reason) {
        _reason = reason;
    }

    bool operator==(const auto& other) const {
        return _code == other.getCode() && _reason == other.getReason();
    }

private:
    static void _extDestroy(::MongoExtensionStatus* status) noexcept {
        delete static_cast<ExtensionGenericStatus*>(status);
    }

    static int32_t _extGetCode(const ::MongoExtensionStatus* status) noexcept {
        return static_cast<const ExtensionGenericStatus*>(status)->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionGenericStatus*>(status)->getReason());
    }

    static void _extSetCode(::MongoExtensionStatus* status, int32_t newCode) noexcept {
        static_cast<ExtensionGenericStatus*>(status)->setCode(newCode);
    }

    static ::MongoExtensionStatus* _extSetReason(::MongoExtensionStatus* status,
                                                 MongoExtensionByteView newReason) noexcept;

    static MongoExtensionStatus* _extClone(const ::MongoExtensionStatus* status,
                                           ::MongoExtensionStatus** output) noexcept;

    static const ::MongoExtensionStatusVTable VTABLE;

    int32_t _code;
    std::string _reason;
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
        delete static_cast<ExtensionStatusException*>(status);
    }

    static int32_t _extGetCode(const ::MongoExtensionStatus* status) noexcept {
        return static_cast<const ExtensionStatusException*>(status)->getCode();
    }

    static MongoExtensionByteView _extGetReason(const ::MongoExtensionStatus* status) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionStatusException*>(status)->getReason());
    }

    static void _extSetCode(::MongoExtensionStatus* status, int32_t newCode) noexcept {
        // No-op for ExtensionStatusException because the wrapped exception is immutable
        // and its error code is set at construction time
    }

    static ::MongoExtensionStatus* _extSetReason(::MongoExtensionStatus* status,
                                                 MongoExtensionByteView newReason) noexcept;

    static MongoExtensionStatus* _extClone(const ::MongoExtensionStatus* status,
                                           ::MongoExtensionStatus** output) noexcept;

    static const ::MongoExtensionStatusVTable VTABLE;

    /**
     * _code may be MONGO_EXTENSION_STATUS_RUNTIME_ERROR if there was no error code associated
     * with the exception.
     */
    int32_t _code;
    std::string _reason;
    std::exception_ptr _exception;
};

class StatusAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionStatus> {
    using CppApi_t = StatusAPI;
};

using StatusHandle = OwnedHandle<::MongoExtensionStatus>;

/**
 * StatusAPI is a wrapper around a MongoExtensionStatus.
 *
 * Typically this is a handle around a MongoExtensionStatus allocated by the host whose
 * ownership has been transferred to the extension. Note that this includes assertion exceptions
 * that are allocated by the host but were triggered/conceptually thrown by the extension.
 */
class StatusAPI : public VTableAPI<::MongoExtensionStatus> {
public:
    StatusAPI(::MongoExtensionStatus* status) : VTableAPI<::MongoExtensionStatus>(status) {}

    /**
     * Return a non-zero code associated with `error`.
     */
    int getCode() const {
        return _vtable().get_code(get());
    }

    /**
     * Return a utf-8 string associated with `MongoExtensionStatus`. May be empty.
     */
    std::string_view getReason() const {
        return byteViewAsStringView(_vtable().get_reason(get()));
    }

    void setCode(int code);

    void setReason(std::string_view reason);

    StatusHandle clone() const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(10930105, "HostStatus 'get_code' is null", vtable.get_code != nullptr);
        tassert(10930106, "HostStatus 'get_reason' is null", vtable.get_reason != nullptr);
        tassert(11186306, "HostStatus 'set_code' is null", vtable.set_code != nullptr);
        tassert(11186309, "HostStatus 'set_reason' is null", vtable.set_reason != nullptr);
        tassert(11186310, "HostStatus 'clone' is null", vtable.clone != nullptr);
    };
};

/**
 * ExtensionDBException represents a MongoExtensionStatus reporting an error, rethrown as a C++
 * exception. When a call is made across the API boundary via the C API, the function must be
 * invoked using invokeCAndConvertStatusToException, which throws a non-OK MongoExtensionStatus as
 * an ExtensionDBException wrapping the original returned status.
 *
 * We hold on to the original status handle in order to facilitate propagating the status across
 * the API boundary multiple times if necessary without needing to re-allocate a
 * MongoExtensionStatus.
 *
 * Exceptions are generally thrown by value, and are either moved or copied depending on the
 * platform. Recent Visual Studio versions mandate all exceptions have a copy constructor, while
 * our supported linux compilers both take advantage of the move semantics. In both these
 * scenarios, it should be safe to extract the status handle from inside a catch block.
 *
 */
class ExtensionDBException final : public DBException {
public:
    using DBException::DBException;
    ExtensionDBException(StatusHandle extensionStatus)
        : DBException(error_details::makeStatus(extensionStatus->getCode(),
                                                std::string(extensionStatus->getReason()))),
          _extensionStatus(std::move(extensionStatus)) {}

    ExtensionDBException(const ExtensionDBException& other)
        : DBException(other), _extensionStatus(other._extensionStatus->clone()) {}

    ExtensionDBException(ExtensionDBException&& other)
        : DBException(std::move(other)),
          _extensionStatus(std::move(other._extensionStatus)) {}  // NOLINT(bugprone-use-after-move)

    ExtensionDBException& operator=(const ExtensionDBException& other) {
        DBException::operator=(other);
        _extensionStatus = other._extensionStatus->clone();
        return *this;
    }

    ExtensionDBException& operator=(ExtensionDBException&& other) {
        DBException::operator=(std::move(other));
        _extensionStatus = std::move(other._extensionStatus);  // NOLINT(bugprone-use-after-move)
        return *this;
    }

    StatusHandle extractStatus() {
        return std::move(_extensionStatus);
    }

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {};
    StatusHandle _extensionStatus;
};

/**
 * ObservabilityContext provides callbacks for success and error cases when invoking
 * C API functions.
 */
class ObservabilityContext {
public:
    virtual ~ObservabilityContext() {};

    virtual void extensionSuccess() const noexcept = 0;
    virtual void extensionError() const noexcept = 0;
    virtual void hostSuccess() const noexcept = 0;
    virtual void hostError() const noexcept = 0;
};

/**
 * getGlobalObservabilityContext returns a pointer to a function scoped static variable. Once
 * this value is set by setObservabilityContext(), it is never expected to change.
 */
const ObservabilityContext* getGlobalObservabilityContext() noexcept;

/**
 * setGlobalObservabilityContext was introduced to accomodate the global ObservabilityContext
 * which is required to provide metrics on the Host. The ObservabilityContext is used as
 * necessary within the wrapCXXAndConvertExceptionToStatus & invokeCAndConvertStatusToException
 * helpers. While the ObservabilityContext has no use on the extension side of the API boundary,
 * it is introduced here because both the Host and the Extension share this file to reduce our
 * maintenance burden.
 *
 * IMPORTANT NOTE: This function must only be called by the Host, and should never be called by
 * extensions.
 * Furthermore, it should never be called from a multi-threaded context, and should only be
 * called once. We could implement a locking mechanism, but given the intended use of the
 * ObsCtx, it was deemed unecessary.
 */
void setGlobalObservabilityContext(std::unique_ptr<ObservabilityContext> obsCtx);

/**
 *
 * Note: This function is only used to allow a workaround for unit tests. It should only be called
 * from a unit test context. Any other use is strictly forbidden and not supported.
 *
 * When extensions are built as part of a unit test, or for a unit test (i.e
 * load_extension_test.cpp), they are not yet statically linked. Ideally, we want to prevent the
 * ObservabilityContext from being set twice. However, due to the linking limitation in unit testing
 * scenarios, we need to provide a workaround for us to manually set the ObservabilityContext to
 * null temporarily when using the $assert extension stage (i.e extension_errors.cpp).
 *
 * This is because the $assert extension performs a sanity check at parse time which checks that
 * the ObservabilityContext is always null. This sanity check is valuable for us when running
 * integration tests directly on the mongod/mongos binaries, but problematic in unit test
 * scenarios.
 *
 * As a workaround, in the $assert extension, we conditionally skip the sanity check if a
 * TestingProctor is initialized and enabled as this indicates the test extension was built into
 * a unit test. This does not work for load_extension_test.cpp, because in that case, the
 * $assert extension does not see the same TestingProctor::instance as the unit test executable,
 * and incorrectly assumes it is not running in a unit test. This is due to the the visibility
 * of the symbols exported by the extension.
 *
 * For this reason, we provide this function to conditionally allow releasing the global
 * ObservabilityContext within the context of a unit test. This works in the case of
 * load_extension_test.cpp because the calls to the TestingProctor within the unit test
 * executable's libraries - with the exception of the extensions - resolve to the same symbol.
 *
 * TODO SERVER-117648: Once we statically link extensions even when they are part of unit tests,
 * remove releaseGlobalObservabilityContext() and all its references.
 */
std::unique_ptr<ObservabilityContext> releaseGlobalObservabilityContext();

enum class MetricsGuardMode : int { kHostSide = 0, kExtensionSide = 1 };

template <MetricsGuardMode GuardMode>
class MetricsGuard {
    static_assert(GuardMode == MetricsGuardMode::kHostSide ||
                  GuardMode == MetricsGuardMode::kExtensionSide);

public:
    MetricsGuard(const ObservabilityContext* obsCtx) noexcept : _obsCtx(obsCtx) {}
    MetricsGuard() noexcept {}
    MetricsGuard(const MetricsGuard&) = delete;
    MetricsGuard& operator=(const MetricsGuard&) = delete;
    MetricsGuard(MetricsGuard&& other) = delete;
    MetricsGuard& operator=(MetricsGuard&& other) = delete;

    ~MetricsGuard() noexcept {
        if (!_obsCtx) {
            return;
        }

        if constexpr (GuardMode == MetricsGuardMode::kHostSide) {
            _success ? _obsCtx->hostSuccess() : _obsCtx->hostError();
        } else if constexpr (GuardMode == MetricsGuardMode::kExtensionSide) {
            _success ? _obsCtx->extensionSuccess() : _obsCtx->extensionError();
        }
    }

    void setContext(const ObservabilityContext* obsCtx) {
        _obsCtx = obsCtx;
    }

    void markSuccess() {
        _success = true;
    }

private:
    const ObservabilityContext* _obsCtx{nullptr};
    bool _success{false};
};

/**
 * wrapCXXAndConvertExceptionToStatus is a template helper that allows functions that will be
 * invoked across the C API boundary to return control to the caller safely.
 *
 * In other words: Since exceptions are not allowed to cross the API boundary, this helper is
 * used in the **implementation of Extension API functionality** (or, "adapters") to ensure that
 * exceptions are caught and translated into a MongoExtensionStatus*, which can be safely
 * returned to the caller. Once the caller receives the status, they may use the helper
 * invokeCAndConvertStatusToException to translate the status back into a C++ exception
 * (assuming the caller is in C++).
 *
 * NOTE: ObservabilityContext is only expected to be non-null when this code is running on the host
 * side of the API, not on the extension. In this function, we update the
 * hostSuccesses/hostFailures metrics, because wrapCXXAndConvertExceptionToStatus, when used by the
 * Host, is used when a call from the extension re-enters the Host side of the API boundary.
 */
template <typename Fn>
::MongoExtensionStatus* wrapCXXAndConvertExceptionToStatus(Fn&& fn) {
    MetricsGuard<MetricsGuardMode::kHostSide> metricsGuard;
    try {
        metricsGuard.setContext(getGlobalObservabilityContext());
        fn();
        metricsGuard.markSuccess();
        return &ExtensionStatusOK::getInstance();
    } catch (ExtensionDBException& e) {
        /**
         * If we caught an ExtensionDBException, we can propagate the underlying extension
         * status. Note that we catch here by non-const reference, because we must mutate the
         * exception object to extract the underlying extension status.
         */
        return e.extractStatus().release();
    } catch (const DBException& e) {
        /**
         * If we caught any other type of exception, we capture the current C++ exception in our
         * custom ExtensionStatusException type. If we ever re-enter into our C++ execution
         * context, we can rethrow the captured exception.
         */
        return new ExtensionStatusException(std::current_exception(), e.code());
    } catch (...) {
        return new ExtensionStatusException(std::current_exception(),
                                            MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
    }
}

void convertStatusToException(StatusHandle status);

/**
 * invokeCAndConvertStatusToException is a template helper that wraps a function that **calls
 * into the C API** and returns a MongoExtensionStatus* (likely within some extension "handle"
 * class). The provided functor must return a MongoExtensionStatus*, which this helper will
 * translate into a C++ exception.
 *
 * NOTE: ObservabilityContext is only expected to be non-null when this code is running on the host
 * side of the API, not on the extension. In this function, we update the
 * extensionSuccesses/extensionFailures metrics, because invokeCAndConvertStatusToException, when
 * used by the Host, is used when the host calls into the extension.
 */
template <typename Fn>
void invokeCAndConvertStatusToException(Fn&& fn) {
    MetricsGuard<MetricsGuardMode::kExtensionSide> metricsGuard(getGlobalObservabilityContext());
    StatusHandle status(fn());
    if (auto code = status->getCode(); MONGO_unlikely(code != MONGO_EXTENSION_STATUS_OK)) {
        return convertStatusToException(std::move(status));
    }
    metricsGuard.markSuccess();
}

inline ::MongoExtensionStatus* ExtensionGenericStatus::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        static_cast<ExtensionGenericStatus*>(status)->setReason(byteViewAsStringView(newReason));
    });
}

inline MongoExtensionStatus* ExtensionGenericStatus::_extClone(
    const ::MongoExtensionStatus* status, ::MongoExtensionStatus** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        tassert(11186300,
                "Received invalid output target for ExtensionGenericStatus::clone",
                output != nullptr);
        const auto& instance = *static_cast<const ExtensionGenericStatus*>(status);
        *output = new ExtensionGenericStatus(instance);
    });
}

inline ::MongoExtensionStatus* ExtensionStatusOK::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    // Forbidden for ExtensionStatusOK
    return wrapCXXAndConvertExceptionToStatus(
        []() { tasserted(11186303, "Calling setReason on ExtensionStatusOK is forbidden!"); });
}

inline MongoExtensionStatus* ExtensionStatusOK::_extClone(
    const ::MongoExtensionStatus* status, ::MongoExtensionStatus** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        tassert(11186301,
                "Received invalid output target for ExtensionStatusOK::clone",
                output != nullptr);
        *output = &ExtensionStatusOK::getInstance();
    });
}

inline ::MongoExtensionStatus* ExtensionStatusException::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    // Forbidden for ExtensionStatusException because the wrapped exception is immutable
    // and its error message is set at construction time
    return wrapCXXAndConvertExceptionToStatus([]() {
        tasserted(11186304, "Calling setReason on ExtensionStatusException is forbidden!");
    });
}

inline MongoExtensionStatus* ExtensionStatusException::_extClone(
    const ::MongoExtensionStatus* status, ::MongoExtensionStatus** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        tassert(11186302,
                "Received invalid output target for ExtensionStatusException::clone",
                output != nullptr);
        const auto& instance = *static_cast<const ExtensionStatusException*>(status);
        *output = new ExtensionStatusException(instance);
    });
}
}  // namespace mongo::extension
