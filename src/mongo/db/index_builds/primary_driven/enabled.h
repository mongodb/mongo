// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/server_options.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class OperationContext;
class VersionContext;
}  // namespace mongo

namespace mongo::index_builds::primary_driven {

/**
 * Returns true if the provider mandates primary-driven index builds or the feature flag is
 * enabled. Uses the supplied FCV snapshot and the operation's decorated version context.
 */
bool enabled(OperationContext* opCtx, const ServerGlobalParams::FCVSnapshot& fcvSnapshot);

/**
 * Returns true if the provider mandates primary-driven index builds or the feature flag is
 * enabled. Uses the supplied version context and FCV snapshot.
 */
bool enabled(OperationContext* opCtx,
             const VersionContext& vCtx,
             const ServerGlobalParams::FCVSnapshot& fcvSnapshot);

}  // namespace mongo::index_builds::primary_driven
