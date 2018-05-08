/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_applier.h"

#include "mongo/db/repl/sync_tail.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

OplogApplier::OplogApplier(executor::TaskExecutor* executor,
                           OplogBuffer* oplogBuffer,
                           Observer* observer,
                           ReplicationCoordinator* replCoord,
                           ReplicationConsistencyMarkers* consistencyMarkers,
                           StorageInterface* storageInterface,
                           const OplogApplier::Options& options,
                           ThreadPool* writerPool)
    : _executor(executor),
      _oplogBuffer(oplogBuffer),
      _observer(observer),
      _replCoord(replCoord),
      _consistencyMarkers(consistencyMarkers),
      _storageInterface(storageInterface),
      _options(options),
      _syncTail(std::make_unique<SyncTail>(
          _observer, _consistencyMarkers, _storageInterface, multiSyncApply, writerPool, options)) {
    invariant(!options.relaxUniqueIndexConstraints);
}

Future<void> OplogApplier::startup() {
    auto future = _promise.getFuture();
    auto callback =
        [ this, promise = _promise.share() ](const CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        log() << "Starting oplog application";
        _syncTail->oplogApplication(_oplogBuffer, _replCoord);
        log() << "Finished oplog application";
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(callback).getStatus());
    return future;
}

void OplogApplier::shutdown() {
    _syncTail->shutdown();
}

/**
 * Pushes operations read from sync source into oplog buffer.
 */
void OplogApplier::enqueue(const Operations& operations) {}

}  // namespace repl
}  // namespace mongo
