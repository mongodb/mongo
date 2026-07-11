// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/util/modules.h"

namespace mongo::query_settings {

/**
 * CommandInvocation hooks marking PQS-supported commands (find/aggregate/distinct, or an explain
 * wrapping one) as awaiting query settings resolution before they run.
 */
class [[MONGO_MOD_PUBLIC]] QuerySettingsCommandHooks : public CommandInvocationHooks {
public:
    void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) override;

    void onAfterRun(OperationContext* opCtx,
                    CommandInvocation* invocation,
                    rpc::ReplyBuilderInterface* response) override {}
};

}  // namespace mongo::query_settings
