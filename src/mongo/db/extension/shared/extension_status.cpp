// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_status.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/util/testing_proctor.h"

#include <string_view>


namespace mongo::extension {

const ::MongoExtensionStatusVTable extension::ExtensionStatusException::VTABLE = {
    .destroy = &ExtensionStatusException::_extDestroy,
    .get_code = &ExtensionStatusException::_extGetCode,
    .get_reason = &ExtensionStatusException::_extGetReason,
    .set_code = &ExtensionStatusException::_extSetCode,
    .set_reason = &ExtensionStatusException::_extSetReason,
    .clone = &ExtensionStatusException::_extClone,
};

const ::MongoExtensionStatusVTable extension::ExtensionGenericStatus::VTABLE = {
    .destroy = &ExtensionGenericStatus::_extDestroy,
    .get_code = &ExtensionGenericStatus::_extGetCode,
    .get_reason = &ExtensionGenericStatus::_extGetReason,
    .set_code = &ExtensionGenericStatus::_extSetCode,
    .set_reason = &ExtensionGenericStatus::_extSetReason,
    .clone = &ExtensionGenericStatus::_extClone,
};

namespace {
// Note, this function should only be called from this file. It's used to ensure the singleton is
// initialized only once (function scope static, one copy per DSO).
std::unique_ptr<ObservabilityContext>& _getGlobalObservabilityContext() noexcept {
    static std::unique_ptr<ObservabilityContext> obsCtx{nullptr};
    return obsCtx;
}
}  // namespace

const ObservabilityContext* getGlobalObservabilityContext() noexcept {
    return _getGlobalObservabilityContext().get();
}

void setGlobalObservabilityContext(std::unique_ptr<ObservabilityContext> obsCtx) {
    auto& currObsCtx = _getGlobalObservabilityContext();
    tassert(11569601,
            "Attempted to call setGlobalObservabilityContext more than once!",
            currObsCtx.get() == nullptr);
    tassert(11569603,
            "Attempted to call setGlobalObservabilityContext with invalid context!",
            obsCtx.get() != nullptr);
    currObsCtx = std::move(obsCtx);
}

// This function is a helper that allows extracting exceptions from a generic MongoExtensionStatus.
// If a new adapter is added in the future (i.e ExtensionStatusException_v1_x), we need to
// accommodate it here.
std::exception_ptr extractExceptionFromExtensionStatusException(
    ::MongoExtensionStatus& status, const CapabilityVersion& capability) {
    tassert(13692203,
            "Unsupported CapabilityVersion for ExtensionStatusException",
            ExtensionStatusException::kCapabilityBounds.isSupported(capability));
    return ExtensionStatusException::extractException(status);
}

void convertStatusToException(StatusHandle status) {
    /**
     * If we can extract an exception pointer from the received status from the C API call, it means
     * that this was originally a C++ exception. In that case, we can go ahead and rethrow that
     * exception here.
     *
     * Otherwise, we received an error from the extension that was not originally from this
     * execution context, so we throw our ExtensionDBException which will allow us to rethrow this
     * exception later if we happen to cross the API boundary again.
     */
    if (auto exceptionPtr = extractExceptionFromExtensionStatusException(
            *status.get(), status->getCapabilityVersion());
        exceptionPtr) {
        std::rethrow_exception(std::move(exceptionPtr));
    }
    throw ExtensionDBException(std::move(status));
}

void StatusAPI::setCode(int code) {
    _vtable().set_code(get(), code);
}

void StatusAPI::setReason(std::string_view reason) {
    auto byteView = stringViewAsByteView(reason);
    invokeCAndConvertStatusToException([&]() { return _vtable().set_reason(get(), byteView); },
                                       getCapabilityVersion());
}

StatusHandle StatusAPI::clone() const {
    ::MongoExtensionStatus* cloneTarget{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().clone(get(), &cloneTarget); },
                                       getCapabilityVersion());
    return StatusHandle(cloneTarget, getCapabilityVersion());
}
}  // namespace mongo::extension
