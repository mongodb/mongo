// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {

class ChangeStreamsClusterParameterStorage;

/**
 * Validates 'changeStreams' cluster-wide parameter.
 * This functionality is only here for downwards-compatibility reasons.
 * The 'changeStreams' cluster-wide parameter has no purpose anymore.
 */
Status validateChangeStreamsClusterParameter(
    const ChangeStreamsClusterParameterStorage& clusterParameter);
}  // namespace mongo
