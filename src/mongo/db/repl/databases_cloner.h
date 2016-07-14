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

#pragma once

#include <list>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OldThreadPool;

namespace repl {
namespace {

using CBHStatus = StatusWith<executor::TaskExecutor::CallbackHandle>;
using CommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

}  // namespace.

/**
 * Clones all databases.
 */
class DatabasesCloner {
public:
    using IncludeDbFilterFn = stdx::function<bool(const BSONObj& dbInfo)>;
    using OnFinishFn = stdx::function<void(const Status&)>;
    DatabasesCloner(StorageInterface* si,
                    executor::TaskExecutor* exec,
                    OldThreadPool* dbWorkThreadPool,
                    HostAndPort source,
                    IncludeDbFilterFn includeDbPred,
                    OnFinishFn finishFn);

    Status startup();
    bool isActive();
    void join();
    void shutdown();

    /**
     * Returns the status after completion. If multiple error occur, only one is recorded/returned.
     *
     * NOTE: A value of ErrorCodes::NotYetInitialized is the default until started.
     */
    Status getStatus();
    std::string toString() const;

private:
    /**
     *  Setting the status to not-OK will stop the process
     */
    void _setStatus_inlock(Status s);

    /**
     * Will fail the cloner, unlock and call the completion function.
     */
    void _failed_inlock(UniqueLock& lk);

    void _cancelCloners_inlock(UniqueLock& lk);

    /** Called each time a database clone is finished */
    void _onEachDBCloneFinish(const Status& status, const std::string& name);

    //  Callbacks

    void _onListDatabaseFinish(const CommandCallbackArgs& cbd);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (M)  Reads and writes guarded by _mutex
    // (S)  Self-synchronizing; access in any way from any context.
    //
    mutable stdx::mutex _mutex;                         // (S)
    Status _status{ErrorCodes::NotYetInitialized, ""};  // (M) If it is not OK, we stop everything.
    executor::TaskExecutor* _exec;                      // (R) executor to schedule things with
    OldThreadPool* _dbWorkThreadPool;  // (R) db worker thread pool for collection cloning.
    HostAndPort _source;               // (R) The source to use, until we get an error
    bool _active = false;              // (M) false until we start
    std::vector<std::shared_ptr<DatabaseCloner>> _databaseCloners;  // (M) database cloners by name
    int _clonersActive = 0;  // (M) Number of active cloners left.
    std::unique_ptr<RemoteCommandRetryScheduler> _listDBsScheduler;  // (M) scheduler for listDBs.

    const IncludeDbFilterFn _includeDbFn;  // (R) function which decides which dbs are cloned.
    const OnFinishFn _finishFn;            // (R) function called when finished.
    StorageInterface* _storage;            // (R)
};


}  // namespace repl
}  // namespace mongo
