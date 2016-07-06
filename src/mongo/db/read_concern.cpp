/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/read_concern.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// This is a special flag that allows for testing of snapshot behavior by skipping the replication
// related checks and isolating the storage/query side of snapshotting.
bool testingSnapshotBehaviorInIsolation = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> TestingSnapshotBehaviorInIsolation(
    ServerParameterSet::getGlobal(),
    "testingSnapshotBehaviorInIsolation",
    &testingSnapshotBehaviorInIsolation);

}  // namespace

StatusWith<repl::ReadConcernArgs> extractReadConcern(OperationContext* txn,
                                                     const BSONObj& cmdObj,
                                                     bool supportsReadConcern) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (!supportsReadConcern && !readConcernArgs.isEmpty()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support read concern"};
    }

    return readConcernArgs;
}

Status waitForReadConcern(OperationContext* txn, const repl::ReadConcernArgs& readConcernArgs) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(txn);

    // Skip waiting for the OpTime when testing snapshot behavior
    if (!testingSnapshotBehaviorInIsolation && !readConcernArgs.isEmpty()) {
        Status status = replCoord->waitUntilOpTimeForRead(txn, readConcernArgs);
        if (!status.isOK()) {
            return status;
        }
    }

    if ((replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet ||
         testingSnapshotBehaviorInIsolation) &&
        (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern ||
         readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern)) {
        // ReadConcern Majority is not supported in ProtocolVersion 0.
        if (!testingSnapshotBehaviorInIsolation && !replCoord->isV1ElectionProtocol()) {
            return {ErrorCodes::ReadConcernMajorityNotEnabled,
                    str::stream() << "Replica sets running protocol version 0 do not support "
                                     "readConcern: majority"};
        }

        const int debugLevel = serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 1 : 2;

        LOG(debugLevel) << "Waiting for 'committed' snapshot to be available for reading: "
                        << readConcernArgs;

        Status status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOG(debugLevel) << "Snapshot not available yet.";
            replCoord->waitUntilSnapshotCommitted(txn, SnapshotName::min());
            status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
        }

        if (!status.isOK()) {
            return status;
        }

        LOG(debugLevel) << "Using 'committed' snapshot: " << CurOp::get(txn)->query();
    }

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (!readConcernArgs.getOpTime().isNull())
            return {ErrorCodes::FailedToParse,
                    "afterOpTime not compatible with linearizable read concern"};

        if (!replCoord->getMemberState().primary())
            return {ErrorCodes::NotMaster,
                    "cannot satisfy linearizable read concern on non-primary node"};
    }

    return Status::OK();
}

}  // namespace mongo
