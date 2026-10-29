// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_command_hooks.h"

#include "mongo/db/client.h"
#include "mongo/db/query/query_settings/query_settings_context.h"

#include <variant>

namespace mongo::query_settings {

void QuerySettingsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                            CommandInvocation* invocation) {
    namespace qsd = query_settings_details;
    // Skip nested DBDirectClient commands: they must observe the user command's resolved settings
    // rather than resolving their own.
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    // Unwrap wrapping invocations (e.g. explain) to reach the command being run.
    const CommandInvocation* unwrapped = invocation;
    while (auto* inner = unwrapped->inner()) {
        unwrapped = inner;
    }
    if (!unwrapped->definition()->supportsQuerySettings()) {
        return;
    }

    auto& state = qsd::getQuerySettingsStateForOp(opCtx);
    if (std::holds_alternative<qsd::NotStarted>(state)) {
        state = qsd::Pending{};
    }
}

}  // namespace mongo::query_settings
