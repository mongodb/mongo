// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/buildinfo_gen.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Populate standard buildInfo content.
 * Note that this does not include the 'storageEngines' field which is specific to Shard roles.
 */
BuildInfo getBuildInfo();

/**
 * Populate just the 'version' and 'versionArray' fields of buildInfo content.
 */
BuildInfo getBuildInfoVersionOnly();

}  // namespace mongo
