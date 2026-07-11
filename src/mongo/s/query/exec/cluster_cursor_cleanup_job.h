// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/background.h"
#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Background job which regularly performs cleanup tasks on the ClusterCursorManager owned by the
 * Grid singleton.
 *
 * Cleanup tasks include:
 * - Killing cursors that have been inactive for some time.
 * - Reaping cursors that have been killed.
 */
class ClusterCursorCleanupJob final : public BackgroundJob {
public:
    std::string name() const final;
    void run() final;
};

extern ClusterCursorCleanupJob clusterCursorCleanupJob;

}  // namespace mongo
