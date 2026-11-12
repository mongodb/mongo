// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/extension/shared/per_version_adapter.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo::extension {

template <uint32_t kPacked>
class ExtensionStatusOK;

/**
 * getExtensionStatusOK returns the ExtensionStatusOK instance for the given negotiated
 * capability version. This trampoline is the only way to obtain an ExtensionStatusOK: the
 * per-version instances cannot be vended without a negotiated capability.
 */
::MongoExtensionStatus* getExtensionStatusOK(const CapabilityVersion& capability);

/**
 * ExtensionStatusOK is an implementation of ::MongoExtensionStatus that is always OK.
 *
 * We use one instance per negotiable capability version to avoid having to allocate a status
 * every time we need to return success across the API boundary. The version is baked into the
 * type (and exposed via kCapability) rather than stored in a member: the negotiated version is
 * session state, and a per-process singleton cannot hold per-session state. Instantiating per
 * version keeps instances immutable and shared by all sessions that negotiated the same version.
 *
 * kPacked must be a valid CapabilityVersion packed value representation.
 */
template <uint32_t kPacked>
class ExtensionStatusOK final : public ::MongoExtensionStatus {
public:
    static constexpr CapabilityVersion kCapability = CapabilityVersion::fromPackedValue(kPacked);

    ~ExtensionStatusOK() {
        sInstanceCount--;
    }

    std::string_view getReason() const {
        return std::string_view();
    }

    int32_t getCode() const {
        return MONGO_EXTENSION_STATUS_OK;
    }

    constexpr const CapabilityVersion& getCapability() const {
        return kCapability;
    }

    static size_t getInstanceCount() {
        return sInstanceCount;
    }

    static ::MongoExtensionStatusVTable getVTable() {
        return VTABLE;
    }

private:
    template <template <uint32_t> class InstanceT, typename CApiT>
    friend CApiT* getNegotiatedVersionInstance(const CapabilityVersion&);

    ExtensionStatusOK() : ::MongoExtensionStatus{&VTABLE} {
        sInstanceCount++;
    }

    // Provide access to our per-version instance.
    static ExtensionStatusOK& getInstance() {
        static ExtensionStatusOK singleton;
        return singleton;
    }

    static ::MongoExtensionStatus* getInstancePtr() {
        return &getInstance();
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

template <uint32_t kPacked>
const ::MongoExtensionStatusVTable ExtensionStatusOK<kPacked>::VTABLE = {
    .destroy = &ExtensionStatusOK::_extDestroy,
    .get_code = &ExtensionStatusOK::_extGetCode,
    .get_reason = &ExtensionStatusOK::_extGetReason,
    .set_code = &ExtensionStatusOK::_extSetCode,
    .set_reason = &ExtensionStatusOK::_extSetReason,
    .clone = &ExtensionStatusOK::_extClone,
};

template <uint32_t kPacked>
size_t ExtensionStatusOK<kPacked>::sInstanceCount = 0;

inline ::MongoExtensionStatus* getExtensionStatusOK(const CapabilityVersion& capability) {
    // One ExtensionStatusOK instantiation per negotiable (major, minor), in a row-major table
    // with one row per supported major and one column per negotiable minor of that major. The
    // table holds factories so each instance is constructed lazily, on first negotiation of its
    // version.
    return getNegotiatedVersionInstance<ExtensionStatusOK, ::MongoExtensionStatus>(capability);
}

/**
 * ExtensionGenericStatus is an implementation of ::MongoExtensionStatus that can be set across the
 * API boundary.
 */
class ExtensionGenericStatus final : public ::MongoExtensionStatus {
public:
    static constexpr CapabilityBounds kCapabilityBounds = CapabilityBounds(
        CapabilityVersion::legacyCapability(), CapabilityVersion::latestCapability());

    ExtensionGenericStatus(CapabilityVersion capability = CapabilityVersion::legacyCapability())
        : ::MongoExtensionStatus{&VTABLE},
          _code(MONGO_EXTENSION_STATUS_OK),
          _reason(""),
          _capability(capability) {
        tassert(13692205,
                "ExtensionGenericStatus constructed with an out-of-bounds capability version",
                isSupported(_capability));
    }

    ExtensionGenericStatus(int32_t code,
                           std::string reason,
                           CapabilityVersion capability = CapabilityVersion::legacyCapability())
        : ::MongoExtensionStatus{&VTABLE}, _code(code), _reason(reason), _capability(capability) {
        tassert(13692206,
                "ExtensionGenericStatus constructed with an out-of-bounds capability version",
                isSupported(_capability));
    }

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

    constexpr const CapabilityVersion& getCapability() const {
        return _capability;
    }

    static constexpr bool isSupported(const CapabilityVersion& capability) {
        return kCapabilityBounds.isSupported(capability);
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
    const CapabilityVersion _capability;
};

/**
 * ExtensionStatus is an abstraction for extensions to provide the status of API calls made into
 * the extension by the host.
 */
class ExtensionStatusException final : public ::MongoExtensionStatus {
public:
    static constexpr CapabilityBounds kCapabilityBounds = CapabilityBounds(
        CapabilityVersion::legacyCapability(), CapabilityVersion::latestCapability());

    ExtensionStatusException(CapabilityVersion capability,
                             std::exception_ptr&& e,
                             int32_t code,
                             const std::string& reason = std::string(""))
        : ::MongoExtensionStatus{&VTABLE},
          _capability(std::move(capability)),
          _code(code),
          _reason(reason),
          _exception(std::move(e)) {
        tassert(13692207,
                "ExtensionStatusException constructed with an out-of-bounds capability version",
                kCapabilityBounds.isSupported(_capability));
    }

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

    constexpr const CapabilityVersion& getCapability() const {
        return _capability;
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

    const CapabilityVersion _capability;
    /**
     * _code may be MONGO_EXTENSION_STATUS_RUNTIME_ERROR if there was no error code associated
     * with the exception.
     */
    int32_t _code;
    std::string _reason;
    std::exception_ptr _exception;
};

template <typename... Args>
inline ::MongoExtensionStatus* createExtensionStatusException(const CapabilityVersion& capability,
                                                              Args&&... args) {
    if (ExtensionStatusException::kCapabilityBounds.isSupported(capability)) {
        return new ExtensionStatusException(capability, std::forward<Args>(args)...);
    }
    return nullptr;
}

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
    StatusAPI(::MongoExtensionStatus* status, const CapabilityVersion& capability)
        : VTableAPI<::MongoExtensionStatus>(status, capability) {}

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

    static void assertVTableConstraints(const VTable_t& vtable, const CapabilityVersion&) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostStatus 'get_code' is null",
                vtable.get_code != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostStatus 'get_reason' is null",
                vtable.get_reason != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostStatus 'set_code' is null",
                vtable.set_code != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostStatus 'set_reason' is null",
                vtable.set_reason != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostStatus 'clone' is null",
                vtable.clone != nullptr);
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

    static constexpr bool isSupported(CapabilityVersion capability) {
        return StatusHandle::isSupported(capability);
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
::MongoExtensionStatus* wrapCXXAndConvertExceptionToStatus(Fn&& fn,
                                                           const CapabilityVersion& capVersion) {
    MetricsGuard<MetricsGuardMode::kHostSide> metricsGuard;
    ::MongoExtensionStatus* errorStatus{nullptr};
    try {
        metricsGuard.setContext(getGlobalObservabilityContext());
        fn();
        metricsGuard.markSuccess();
        return getExtensionStatusOK(capVersion);
    } catch (ExtensionDBException& e) {
        /**
         * If we caught an ExtensionDBException, we can propagate the underlying extension
         * status. Note that we catch here by non-const reference, because we must mutate the
         * exception object to extract the underlying extension status.
         */
        errorStatus = e.extractStatus().release();
    } catch (const DBException& e) {
        /**
         * If we caught any other type of exception, we capture the current C++ exception in our
         * custom ExtensionStatusException type. If we ever re-enter into our C++ execution
         * context, we can rethrow the captured exception.
         */
        errorStatus =
            createExtensionStatusException(capVersion, std::current_exception(), e.code());
    } catch (...) {
        errorStatus = createExtensionStatusException(
            capVersion, std::current_exception(), MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
    }
    tassert(13692202,
            "Unable to create ExtensionStatusException, invalid capVersion",
            errorStatus != nullptr);
    return errorStatus;
}

void convertStatusToException(StatusHandle status);

/**
 * invokeCAndConvertStatusToException is a template helper that wraps a function that **calls
 * into the C API** and returns a MongoExtensionStatus* (likely within some extension "handle"
 * class). The provided functor must return a MongoExtensionStatus*, which this helper will
 * translate into a C++ exception.
 *
 * NOTE: ObservabilityContext is only expected to be non-null when this code is running on the
 * host side of the API, not on the extension. In this function, we update the
 * extensionSuccesses/extensionFailures metrics, because invokeCAndConvertStatusToException,
 * when used by the Host, is used when the host calls into the extension.
 */
template <typename Fn>
void invokeCAndConvertStatusToException(Fn&& fn, const CapabilityVersion& capability) {
    if (MONGO_likely(StatusHandle::isSupported(capability))) {
        MetricsGuard<MetricsGuardMode::kExtensionSide> metricsGuard(
            getGlobalObservabilityContext());
        StatusHandle status(fn(), capability);
        if (auto code = status->getCode(); MONGO_unlikely(code != MONGO_EXTENSION_STATUS_OK)) {
            return convertStatusToException(std::move(status));
        }
        metricsGuard.markSuccess();
    } else {
        tasserted(13692201, "Unsupported capability for invokeCAndConvertStatusToException");
    }
}

inline ::MongoExtensionStatus* ExtensionGenericStatus::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    auto* genericStatus = static_cast<ExtensionGenericStatus*>(status);
    return wrapCXXAndConvertExceptionToStatus(
        [&]() { genericStatus->setReason(byteViewAsStringView(newReason)); },
        genericStatus->getCapability());
}

inline MongoExtensionStatus* ExtensionGenericStatus::_extClone(
    const ::MongoExtensionStatus* status, MongoExtensionStatus** output) noexcept {
    const auto* genericStatus = static_cast<const ExtensionGenericStatus*>(status);
    return wrapCXXAndConvertExceptionToStatus(
        [&]() {
            tassert(11186300,
                    "Received invalid output target for ExtensionGenericStatus::clone",
                    output != nullptr);
            *output = new ExtensionGenericStatus(*genericStatus);
        },
        genericStatus->getCapability());
}

template <uint32_t kPacked>
inline ::MongoExtensionStatus* ExtensionStatusOK<kPacked>::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    // Forbidden for ExtensionStatusOK
    return wrapCXXAndConvertExceptionToStatus(
        []() { tasserted(11186303, "Calling setReason on ExtensionStatusOK is forbidden!"); },
        kCapability);
}

template <uint32_t kPacked>
inline MongoExtensionStatus* ExtensionStatusOK<kPacked>::_extClone(
    const ::MongoExtensionStatus* status, ::MongoExtensionStatus** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus(
        [&]() {
            tassert(11186301,
                    "Received invalid output target for ExtensionStatusOK::clone",
                    output != nullptr);
            // A clone of an OK status is the same per-version instance.
            *output = &getInstance();
        },
        kCapability);
}

inline ::MongoExtensionStatus* ExtensionStatusException::_extSetReason(
    ::MongoExtensionStatus* status, MongoExtensionByteView newReason) noexcept {
    // Forbidden for ExtensionStatusException because the wrapped exception is immutable
    // and its error message is set at construction time
    const auto* exceptionStatus = static_cast<const ExtensionStatusException*>(status);
    return wrapCXXAndConvertExceptionToStatus(
        []() {
            tasserted(11186304, "Calling setReason on ExtensionStatusException is forbidden!");
        },
        exceptionStatus->getCapability());
}

inline MongoExtensionStatus* ExtensionStatusException::_extClone(
    const ::MongoExtensionStatus* status, ::MongoExtensionStatus** output) noexcept {
    const auto& instance = *static_cast<const ExtensionStatusException*>(status);
    return wrapCXXAndConvertExceptionToStatus(
        [&]() {
            tassert(11186302,
                    "Received invalid output target for ExtensionStatusException::clone",
                    output != nullptr);
            *output = new ExtensionStatusException(instance);
        },
        instance.getCapability());
}
}  // namespace mongo::extension
