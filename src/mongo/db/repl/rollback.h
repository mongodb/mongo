// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status_with.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo {
namespace repl {

/**
 * This class represents the interface BackgroundSync and ReplicationCoordinatorExternalState use to
 * interact with the rollback subsystem.
 */
class [[MONGO_MOD_PRIVATE]] Rollback {
    Rollback(const Rollback&) = delete;
    Rollback& operator=(const Rollback&) = delete;

public:
    /**
     * Callback function to report results of rollback. On success, the last optime applied will be
     * passed in.
     */
    using OnCompletionFn =
        std::function<void(const StatusWith<OpTime>& lastOpTimeApplied) noexcept>;

    Rollback() = default;

    virtual ~Rollback() = default;
};

}  // namespace repl
}  // namespace mongo
