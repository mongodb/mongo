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
#include "mongo/base/status_with.h"
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
    struct Stats {
        size_t databasesCloned{0};
        std::vector<DatabaseCloner::Stats> databaseStats;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    using IncludeDbFilterFn = stdx::function<bool(const BSONObj& dbInfo)>;
    using OnFinishFn = stdx::function<void(const Status&)>;
    DatabasesCloner(StorageInterface* si,
                    executor::TaskExecutor* exec,
                    OldThreadPool* dbWorkThreadPool,
                    HostAndPort source,
                    IncludeDbFilterFn includeDbPred,
                    OnFinishFn finishFn);

    ~DatabasesCloner();

    Status startup() noexcept;
    bool isActive();
    void join();
    void shutdown();
    DatabasesCloner::Stats getStats() const;

    /**
     * Returns the status after completion. If multiple error occur, only one is recorded/returned.
     *
     * NOTE: A value of ErrorCodes::NotYetInitialized is the default until started.
     */
    Status getStatus();
    std::string toString() const;

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& scheduleDbWorkFn);

    /**
     * Calls DatabasesCloner::_setAdminAsFirst.
     * For testing only.
     */
    void setAdminAsFirst_forTest(std::vector<BSONElement>& dbsArray);

    /**
     * Calls DatabasesCloner::_parseListDatabasesResponse.
     * For testing only.
     */
    StatusWith<std::vector<BSONElement>> parseListDatabasesResponse_forTest(BSONObj dbResponse);

private:
    bool _isActive_inlock() const;

    /**
     * Returns a copy of the database cloners.
     */
    std::vector<std::shared_ptr<DatabaseCloner>> _getDatabaseCloners() const;

    /**
     * Returns scheduler for listDatabases (null if not created).
     */
    RemoteCommandRetryScheduler* _getListDatabasesScheduler() const;

    /**
     *  Setting the status to not-OK will stop the process
     */
    void _setStatus_inlock(Status s);

    /** Will fail the cloner, call the completion function, and become inactive. */
    void _fail_inlock(UniqueLock* lk, Status s);

    /** Will call the completion function, and become inactive. */
    void _succeed_inlock(UniqueLock* lk);

    /** Called each time a database clone is finished */
    void _onEachDBCloneFinish(const Status& status, const std::string& name);

    //  Callbacks

    void _onListDatabaseFinish(const CommandCallbackArgs& cbd);

    /**
     * Takes a vector of BSONElements and scans for an element that contains a 'name' field with the
     * value 'admin'. If found, the element is swapped with the first element in the vector.
     * Otherwise, return.
     *
     * Used to parse the BSONResponse returned by listDatabases.
     */
    void _setAdminAsFirst(std::vector<BSONElement>& dbsArray);

    /**
     * Takes a 'listDatabases' command response and parses the response into a
     * vector of BSON elements.
     *
     * If the input response is malformed, Status ErrorCodes::BadValue will be returned.
     */
    StatusWith<std::vector<BSONElement>> _parseListDatabasesResponse(BSONObj dbResponse);

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
    const HostAndPort _source;         // (R) The source to use.
    CollectionCloner::ScheduleDbWorkFn _scheduleDbWorkFn;  // (M)

    const IncludeDbFilterFn _includeDbFn;  // (R) function which decides which dbs are cloned.
    OnFinishFn _finishFn;                  // (M) function called when finished.
    StorageInterface* _storage;            // (R)

    std::unique_ptr<RemoteCommandRetryScheduler> _listDBsScheduler;  // (M) scheduler for listDBs.
    std::vector<std::shared_ptr<DatabaseCloner>> _databaseCloners;   // (M) database cloners by name
    Stats _stats;                                                    // (M)

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };
    State _state = State::kPreStart;
};


}  // namespace repl
}  // namespace mongo
