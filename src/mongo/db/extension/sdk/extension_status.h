// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/negotiated_capability.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo::extension::sdk {

/**
 * Extension-side overloads of the status conversion helpers that supply the negotiated capability
 * version implicitly.
 *
 * The host may hold several extensions open at several different negotiated versions, so on the
 * host side the capability genuinely varies from call to call and must be passed explicitly. An
 * extension library is the opposite: it negotiates exactly one version for its entire lifetime
 * (see negotiated_capability.h), so every call site in the SDK would otherwise pass the same
 * negotiatedCapabilityVersion() expression. These overloads make that the default, which keeps
 * adapters from having to restate an invariant that holds for the whole library.
 */
using mongo::extension::invokeCAndConvertStatusToException;
using mongo::extension::wrapCXXAndConvertExceptionToStatus;

template <typename Fn>
::MongoExtensionStatus* wrapCXXAndConvertExceptionToStatus(Fn&& fn) {
    return mongo::extension::wrapCXXAndConvertExceptionToStatus(std::forward<Fn>(fn),
                                                                negotiatedCapabilityVersion());
}

template <typename Fn>
void invokeCAndConvertStatusToException(Fn&& fn) {
    mongo::extension::invokeCAndConvertStatusToException(std::forward<Fn>(fn),
                                                         negotiatedCapabilityVersion());
}

}  // namespace mongo::extension::sdk
