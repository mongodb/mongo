/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

class StorageInterface;

class CollectionCloner : public BaseCloner {
    MONGO_DISALLOW_COPYING(CollectionCloner);

public:
    struct Stats {
        Date_t start;
        Date_t end;
        size_t documents{0};
        size_t indexes{0};
        size_t fetchBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
    };
    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = stdx::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn&)>;

    /**
     * Creates CollectionCloner task in inactive state. Use start() to activate cloner.
     *
     * The cloner calls 'onCompletion' when the collection cloning has completed or failed.
     *
     * 'onCompletion' will be called exactly once.
     *
     * Takes ownership of the passed StorageInterface object.
     */
    CollectionCloner(executor::TaskExecutor* executor,
                     const HostAndPort& source,
                     const NamespaceString& sourceNss,
                     const CollectionOptions& options,
                     const CallbackFn& onCompletion,
                     StorageInterface* storageInterface);

    virtual ~CollectionCloner();

    const NamespaceString& getSourceNamespace() const;

    std::string getDiagnosticString() const override;

    bool isActive() const override;

    Status start() override;

    void cancel() override;

    void wait() override;

    CollectionCloner::Stats getStats() const;

    //
    // Testing only functions below.
    //

    /**
     * Waits for database worker to complete.
     * Returns immediately if collection cloner is not active.
     *
     * For testing only.
     */
    void waitForDbWorker();

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn(const ScheduleDbWorkFn& scheduleDbWorkFn);

private:
    /**
     * Read index specs from listIndexes result.
     */
    void _listIndexesCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                              Fetcher::NextAction* nextAction,
                              BSONObjBuilder* getMoreBob);

    /**
     * Read collection documents from find result.
     */
    void _findCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob);

    /**
     * Request storage interface to create collection.
     *
     * Called multiple times if there are more than one batch of responses from listIndexes
     * cursor.
     *
     * 'nextAction' is an in/out arg indicating the next action planned and to be taken
     *  by the fetcher.
     */
    void _beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& callbackData);

    /**
     * Called multiple times if there are more than one batch of documents from the fetcher.
     * On the last batch, 'lastBatch' will be true.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void _insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& callbackData,
                                  bool lastBatch);

    /**
     * Reports completion status.
     * Commits/aborts collection building.
     * Sets cloner to inactive.
     */
    void _finishCallback(const Status& status);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (M)  Reads and writes guarded by _mutex
    // (S)  Self-synchronizing; access in any way from any context.
    // (RT)  Read-only in concurrent operation; synchronized externally by tests
    //
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;        // (M)
    executor::TaskExecutor* _executor;                  // (R) Not owned by us.
    HostAndPort _source;                                // (R)
    NamespaceString _sourceNss;                         // (R)
    NamespaceString _destNss;                           // (R)
    CollectionOptions _options;                         // (R)
    std::unique_ptr<CollectionBulkLoader> _collLoader;  // (M)
    CallbackFn _onCompletion;             // (R) Invoked once when cloning completes or fails.
    StorageInterface* _storageInterface;  // (R) Not owned by us.
    bool _active;                         // (M) true when Collection Cloner is started.
    Fetcher _listIndexesFetcher;          // (S)
    Fetcher _findFetcher;                 // (S)
    std::vector<BSONObj> _indexSpecs;     // (M)
    BSONObj _idIndexSpec;                 // (M)
    std::vector<BSONObj> _documents;      // (M) Documents read from fetcher to insert.
    OldThreadPool _dbWorkThreadPool;      // (R)
    TaskRunner _dbWorkTaskRunner;         // (R)
    ScheduleDbWorkFn
        _scheduleDbWorkFn;  // (RT) Function for scheduling database work using the executor.
    Stats _stats;           // (M) stats for this instance.
};

}  // namespace repl
}  // namespace mongo
