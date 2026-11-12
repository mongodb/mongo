// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host_connector/adapter/execution_context_base.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo::extension::host_connector {

/**
 * CommandExecutionContextBase defines the abstract interface for the command execution context
 * that extensions need while a command's run() executes. It extends ExecutionContextBase, which
 * provides interrupt checks and the operation deadline.
 *
 * Concrete implementations bridge the extension framework to the host process's command execution
 * state, encapsulating access to the command's OperationContext.
 *
 * Implementations of this interface should be wrapped by CommandExecutionContextAdapter for
 * exposure to the public extension API.
 */
class CommandExecutionContextBase : public ExecutionContextBase {
public:
    CommandExecutionContextBase() = default;
    ~CommandExecutionContextBase() override = default;
};

/**
 * CommandExecutionContextAdapter is an adapter to ::MongoExtensionCommandExecutionContext,
 * providing host execution context methods to extension commands.
 */
class CommandExecutionContextAdapter final : public ::MongoExtensionCommandExecutionContext {
public:
    CommandExecutionContextAdapter(std::unique_ptr<CommandExecutionContextBase> ctx,
                                   CapabilityVersion version)
        : ::MongoExtensionCommandExecutionContext{&VTABLE},
          _ctx(std::move(ctx)),
          _capability(std::move(version)) {
        tassert(13729700, "Provided CommandExecutionContextBase is null", _ctx != nullptr);
    }

    CommandExecutionContextAdapter(const CommandExecutionContextAdapter&) = delete;
    CommandExecutionContextAdapter& operator=(const CommandExecutionContextAdapter&) = delete;
    CommandExecutionContextAdapter(CommandExecutionContextAdapter&&) = delete;
    CommandExecutionContextAdapter& operator=(CommandExecutionContextAdapter&&) = delete;

    const CommandExecutionContextBase& getCtxImpl() const {
        return *_ctx;
    }

private:
    static const CapabilityVersion& _capabilityOf(
        const ::MongoExtensionCommandExecutionContext* ctx) noexcept {
        return static_cast<const CommandExecutionContextAdapter*>(ctx)->_capability;
    }

    static MongoExtensionStatus* _extCheckForInterrupt(
        const MongoExtensionCommandExecutionContext* ctx, MongoExtensionStatus* opStatus) noexcept;

    static MongoExtensionStatus* _extGetDeadlineTimestampMs(
        const MongoExtensionCommandExecutionContext* ctx, int64_t* deadlineTimestampMs) noexcept;

    static constexpr ::MongoExtensionCommandExecutionContextVTable VTABLE = {
        .check_for_interrupt = &_extCheckForInterrupt,
        .get_deadline_timestamp_ms = &_extGetDeadlineTimestampMs,
    };

    std::unique_ptr<CommandExecutionContextBase> _ctx;
    const CapabilityVersion _capability;
};

}  // namespace mongo::extension::host_connector
