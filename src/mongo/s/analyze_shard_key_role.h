// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace analyze_shard_key {

bool isFeatureFlagEnabled(bool ignoreFCV = false);

bool supportsCoordinatingQueryAnalysis(bool isReplEnabled);
bool supportsCoordinatingQueryAnalysis(OperationContext* opCtx);

bool supportsPersistingSampledQueries(bool isReplEnabled);
bool supportsPersistingSampledQueries(OperationContext* opCtx);

bool supportsSamplingQueries(bool isReplEnabled);
bool supportsSamplingQueries(OperationContext* opCtx);
bool supportsSamplingQueries(ServiceContext* serviceContext);

}  // namespace analyze_shard_key
}  // namespace mongo
