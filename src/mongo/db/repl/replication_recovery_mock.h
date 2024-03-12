/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/repl/replication_recovery.h"

namespace mongo {
class OperationContext;
namespace repl {

class ReplicationRecoveryMock : public ReplicationRecovery {
    ReplicationRecoveryMock(const ReplicationRecoveryMock&) = delete;
    ReplicationRecoveryMock& operator=(const ReplicationRecoveryMock&) = delete;

public:
    ReplicationRecoveryMock() = default;

    boost::optional<Timestamp> recoverFromOplog(
        OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) override {
        return stableTimestamp;
    }

    void recoverFromOplogAsStandalone(OperationContext* opCtx,
                                      bool duringInitialSync = false) override {}

    void recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) override {}

    void truncateOplogToTimestamp(OperationContext* opCtx,
                                  Timestamp truncateAfterTimestamp) override {}

    void applyOplogEntriesForRestore(OperationContext* opCtx, Timestamp stableTimestamp) override {}
};

}  // namespace repl
}  // namespace mongo
