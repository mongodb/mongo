// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A CommandInvocation hooks to track commands directly targeting system.buckets collections.
 */
class SystemBucketsMetricsCommandHooks final : public CommandInvocationHooks {
public:
    void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) override;
    void onAfterRun(OperationContext* opCtx,
                    CommandInvocation* invocation,
                    rpc::ReplyBuilderInterface* response) override {}

    logv2::SeveritySuppressor _logSuppressor{
        Hours{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
};

}  // namespace mongo
