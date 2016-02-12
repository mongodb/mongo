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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/chunk_move_write_concern_options.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const int kDefaultWriteTimeoutForMigrationMs = 60 * 1000;
const WriteConcernOptions DefaultWriteConcernForMigration(2,
                                                          WriteConcernOptions::SyncMode::NONE,
                                                          kDefaultWriteTimeoutForMigrationMs);

WriteConcernOptions getDefaultWriteConcernForMigration() {
    repl::ReplicationCoordinator* replCoordinator = repl::getGlobalReplicationCoordinator();
    if (replCoordinator->getReplicationMode() == mongo::repl::ReplicationCoordinator::modeReplSet) {
        Status status =
            replCoordinator->checkIfWriteConcernCanBeSatisfied(DefaultWriteConcernForMigration);
        if (status.isOK()) {
            return DefaultWriteConcernForMigration;
        }
    }

    return WriteConcernOptions(1, WriteConcernOptions::SyncMode::NONE, 0);
}

}  // namespace

ChunkMoveWriteConcernOptions::ChunkMoveWriteConcernOptions(BSONObj secThrottleObj,
                                                           WriteConcernOptions writeConcernOptions)
    : _secThrottleObj(std::move(secThrottleObj)),
      _writeConcernOptions(std::move(writeConcernOptions)) {}

StatusWith<ChunkMoveWriteConcernOptions> ChunkMoveWriteConcernOptions::initFromCommand(
    const BSONObj& obj) {
    BSONObj secThrottleObj;
    WriteConcernOptions writeConcernOptions;

    Status status = writeConcernOptions.parseSecondaryThrottle(obj, &secThrottleObj);
    if (!status.isOK()) {
        if (status.code() != ErrorCodes::WriteConcernNotDefined) {
            return status;
        }

        writeConcernOptions = getDefaultWriteConcernForMigration();
    } else {
        repl::ReplicationCoordinator* replCoordinator = repl::getGlobalReplicationCoordinator();

        if (replCoordinator->getReplicationMode() ==
                repl::ReplicationCoordinator::modeMasterSlave &&
            writeConcernOptions.shouldWaitForOtherNodes()) {
            warning() << "moveChunk cannot check if secondary throttle setting "
                      << writeConcernOptions.toBSON()
                      << " can be enforced in a master slave configuration";
        }

        Status status = replCoordinator->checkIfWriteConcernCanBeSatisfied(writeConcernOptions);
        if (!status.isOK() && status != ErrorCodes::NoReplicationEnabled) {
            return status;
        }
    }

    if (writeConcernOptions.shouldWaitForOtherNodes() &&
        writeConcernOptions.wTimeout == WriteConcernOptions::kNoTimeout) {
        // Don't allow no timeout
        writeConcernOptions.wTimeout = kDefaultWriteTimeoutForMigrationMs;
    }

    return ChunkMoveWriteConcernOptions(secThrottleObj, writeConcernOptions);
}

}  // namespace mongo
