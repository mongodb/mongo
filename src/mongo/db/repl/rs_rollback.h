/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBClientConnection;
class NamespaceString;
class OperationContext;

namespace repl {

class OplogInterface;
class OpTime;
class ReplicationCoordinator;
class RollbackSource;

/**
 * Initiates the rollback process.
 * This function assumes the preconditions for undertaking rollback have already been met;
 * we have ops in our oplog that our sync source does not have, and we are not currently
 * PRIMARY.
 * The rollback procedure is:
 * - find the common point between this node and its sync source
 * - undo operations by fetching all documents affected, then replaying
 *   the sync source's oplog until we reach the time in the oplog when we fetched the last
 *   document.
 * This function can throw std::exception on failures.
 * This function runs a command on the sync source to detect if the sync source rolls back
 * while our rollback is in progress.
 *
 * @param txn Used to read and write from this node's databases
 * @param localOplog reads the oplog on this server.
 * @param rollbackSource interface for sync source:
 *            provides oplog; and
 *            supports fetching documents and copying collections.
 * @param replCoord Used to track the rollback ID and to change the follower state
 *
 * Failures: Most failures are returned as a status but some failures throw an std::exception.
 */

using SleepSecondsFn = stdx::function<void(Seconds)>;

Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    ReplicationCoordinator* replCoord,
                    const SleepSecondsFn& sleepSecondsFn);

Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    ReplicationCoordinator* replCoord);

}  // namespace repl
}  // namespace mongo
