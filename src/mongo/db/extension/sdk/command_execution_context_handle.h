// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::extension {
namespace sdk {
class CommandExecutionContextAPI;
}

template <>
struct c_api_to_cpp_api<::MongoExtensionCommandExecutionContext> {
    using CppApi_t = sdk::CommandExecutionContextAPI;
};

// Command execution contexts were introduced with commands in API v1.1.
template <>
struct c_api_to_min_supported_capability<::MongoExtensionCommandExecutionContext>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(CapabilityVersion(1, 1))> {};

namespace sdk {
/**
 * Wrapper for ::MongoExtensionCommandExecutionContext, providing safe access to its public API
 * through the underlying vtable.
 *
 * Typically ownership of the MongoExtensionCommandExecutionContext pointer is never transferred to
 * the extension, so this API is expected to only be used with an UnownedHandle.
 */
class CommandExecutionContextAPI : public VTableAPI<::MongoExtensionCommandExecutionContext> {
public:
    CommandExecutionContextAPI(::MongoExtensionCommandExecutionContext* ctx,
                               const CapabilityVersion& capability)
        : VTableAPI<::MongoExtensionCommandExecutionContext>(ctx, capability) {}

    /**
     * Returns a non-OK status if the command's operation has been interrupted (e.g. killOp,
     * maxTimeMS expiry, or shutdown), and an OK status otherwise.
     */
    ExtensionGenericStatus checkForInterrupt() const;

    /**
     * Returns the operation's deadline as a UNIX timestamp in milliseconds, or the maximum
     * representable int64_t value if the operation has no deadline.
     */
    int64_t getDeadlineTimestampMs() const;

    static void assertVTableConstraints(const VTable_t& vtable, const CapabilityVersion&) {
        sdk_tassert(13729701,
                    "CommandExecutionContext 'check_for_interrupt' is null",
                    vtable.check_for_interrupt != nullptr);
        sdk_tassert(13729702,
                    "CommandExecutionContext 'get_deadline_timestamp_ms' is null",
                    vtable.get_deadline_timestamp_ms != nullptr);
    }
};

using CommandExecutionContextHandle = UnownedHandle<const ::MongoExtensionCommandExecutionContext>;
}  // namespace sdk
}  // namespace mongo::extension
