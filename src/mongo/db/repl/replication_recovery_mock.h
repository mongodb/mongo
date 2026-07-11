// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/replication_recovery.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo {
class OperationContext;
namespace repl {

class [[MONGO_MOD_PUBLIC]] ReplicationRecoveryMock : public ReplicationRecovery {
    ReplicationRecoveryMock(const ReplicationRecoveryMock&) = delete;
    ReplicationRecoveryMock& operator=(const ReplicationRecoveryMock&) = delete;

public:
    ReplicationRecoveryMock() = default;

    boost::optional<Timestamp> recoverFromOplog(
        OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) override {
        if (recoverFromOplogFn) {
            recoverFromOplogFn(opCtx, stableTimestamp);
        }
        return stableTimestamp;
    }

    std::function<void(OperationContext*, boost::optional<Timestamp>)> recoverFromOplogFn;

    void recoverFromOplogAsStandalone(OperationContext* opCtx,
                                      bool duringInitialSync = false) override {
        // Pass through to recoverFromOplog as we do in the cases being tested with this function.
        recoverFromOplog(opCtx, boost::none);
    }

    void recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) override {}

    void truncateOplogToTimestamp(OperationContext* opCtx,
                                  Timestamp truncateAfterTimestamp) override {}

    void applyOplogEntriesForRestore(OperationContext* opCtx, Timestamp stableTimestamp) override {}
};

}  // namespace repl
}  // namespace mongo
