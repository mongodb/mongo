/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

inline const int kProgressMeterSecondsBetween = 60;
inline const int kProgressMeterCheckInterval = 128;

class CollectionCloner final : public InitialSyncBaseCloner {
public:
    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        NamespaceString nss;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t fetchedBatches{0};  // This is actually inserted batches.
        size_t receivedBatches{0};
        long long bytesToCopy{0};
        long long avgObjSize{0};
        long long approxBytesCopied{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = unique_function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        executor::TaskExecutor::CallbackFn)>;

    CollectionCloner(const NamespaceString& ns,
                     const CollectionOptions& collectionOptions,
                     InitialSyncSharedData* sharedData,
                     const HostAndPort& source,
                     DBClientConnection* client,
                     StorageInterface* storageInterface,
                     ThreadPool* dbPool);

    ~CollectionCloner() override = default;

    /**
     * Waits for any database work to finish or fail.
     */
    void waitForDatabaseWorkToComplete();

    Stats getStats() const;

    std::string toString() const;

    NamespaceString getSourceNss() const {
        return _sourceNss;
    }
    UUID getSourceUuid() const {
        return _sourceDbAndUuid.uuid();
    }

    /**
     * Set the cloner batch size.
     *
     * Used for testing only.  Set by server parameter 'collectionClonerBatchSize' in normal
     * operation.
     */
    void setBatchSize_forTest(int batchSize) {
        _collectionClonerBatchSize = batchSize;
    }

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(ScheduleDbWorkFn scheduleDbWorkFn) {
        _scheduleDbWorkFn = std::move(scheduleDbWorkFn);
    }

protected:
    ClonerStages getStages() final;

    bool isMyFailPoint(const BSONObj& data) const final;

private:
    friend class CollectionClonerTest;

    class CollectionClonerStage : public ClonerStage<CollectionCloner> {
    public:
        CollectionClonerStage(std::string name, CollectionCloner* cloner, ClonerRunFn stageFunc)
            : ClonerStage<CollectionCloner>(name, cloner, stageFunc) {}
        AfterStageBehavior run() override;
    };

    class CollectionClonerQueryStage : public CollectionClonerStage {
    public:
        CollectionClonerQueryStage(std::string name,
                                   CollectionCloner* cloner,
                                   ClonerRunFn stageFunc)
            : CollectionClonerStage(name, cloner, stageFunc) {}

        bool isTransientError(const Status& status) override {
            if (isCursorError(status)) {
                return true;
            }
            return ErrorCodes::isRetriableError(status);
        }

        static bool isCursorError(const Status& status) {
            // Our cursor was killed due to changes on the remote collection.
            if ((status == ErrorCodes::CursorNotFound) || (status == ErrorCodes::OperationFailed) ||
                (status == ErrorCodes::QueryPlanKilled)) {
                return true;
            }
            return false;
        }
    };

    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        return toStringForLogging(_sourceNss.dbName()) + " db: { " + stage->getName() +
            ": UUID(\"" + _sourceDbAndUuid.uuid().toString() + "\") coll: " + _sourceNss.coll() +
            " }";
    }

    /**
     * The preStage sets the start time in _stats.
     */
    void preStage() final;

    /**
     * The postStage sets the end time in _stats.
     */
    void postStage() final;

    /**
     * Stage function that runs the collStats command on the collection.
     */
    AfterStageBehavior collStatsStage();

    /**
     * Stage function that counts the number of documents in the collection on the source in order
     * to generate progress information.
     */
    AfterStageBehavior countStage();

    /**
     * Stage function that gets the index information of the collection on the source to re-create
     * it.
     */
    AfterStageBehavior listIndexesStage();

    /**
     * Stage function that creates the collection using the storageInterface.  This stage does not
     * actually contact the sync source.
     */
    AfterStageBehavior createCollectionStage();

    /**
     * Stage function that executes a query to retrieve all documents in the collection.  For each
     * batch returned by the upstream node, handleNextBatch will be called with the data.  This
     * stage will finish when the entire query is finished or failed.
     */
    AfterStageBehavior queryStage();

    /**
     * Stage function that sets up index builders for any unfinished two-phase index builds.
     */
    AfterStageBehavior setupIndexBuildersForUnfinishedIndexesStage();

    /**
     * Put all results from a query batch into a buffer to be inserted, and schedule it to be
     * inserted.
     */
    void handleNextBatch(DBClientCursor& cursor);

    /**
     * Called whenever there is a new batch of documents ready from the DBClientConnection.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd);

    /**
     * Sends a query command to the source. That query command with be parameterized based on
     * wire version and clone progress.
     */
    void runQuery();

    /**
     * Drops the collection if it was dropped on the sync source, if we'd created it and not
     * finished loading it.
     */
    void _maybeDropCollectionOnSyncSourceDrop();

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    const NamespaceString _sourceNss;            // (R)
    const CollectionOptions _collectionOptions;  // (R)
    // Despite the type name, this member must always contain a UUID.
    NamespaceStringOrUUID _sourceDbAndUuid;  // (R)
    // The size of the batches of documents returned in collection cloning.
    int _collectionClonerBatchSize;  // (R)

    CollectionClonerStage _collStatsStage;                               // (R)
    CollectionClonerStage _countStage;                                   // (R)
    CollectionClonerStage _listIndexesStage;                             // (R)
    CollectionClonerStage _createCollectionStage;                        // (R)
    CollectionClonerQueryStage _queryStage;                              // (R)
    CollectionClonerStage _setupIndexBuildersForUnfinishedIndexesStage;  // (R)

    ProgressMeter _progressMeter;                       // (X) progress meter for this instance.
    std::vector<BSONObj> _readyIndexSpecs;              // (X) Except for _id_
    std::vector<BSONObj> _unfinishedIndexSpecs;         // (X)
    BSONObj _idIndexSpec;                               // (X)
    std::unique_ptr<CollectionBulkLoader> _collLoader;  // (X)
    //  Function for scheduling database work using the executor.
    ScheduleDbWorkFn _scheduleDbWorkFn;  // (R)
    // Documents read from source to insert.
    std::vector<BSONObj> _documentsToInsert;  // (M)
    Stats _stats;                             // (M)
    // Putting _dbWorkTaskRunner last ensures anything the database work threads depend on,
    // like _documentsToInsert, is destroyed after those threads exit.
    TaskRunner _dbWorkTaskRunner;  // (R)

    // The resumeToken used to resume after network error.
    boost::optional<BSONObj> _resumeToken;  // (X)

    // The cursorId of the remote collection cursor.
    long long _remoteCursorId = -1;  // (X)

    // If true, it means we are starting a new query or resuming an interrupted one.
    bool _firstBatchOfQueryRound = true;  // (X)

    // Only set during non-resumable (4.2) queries.
    // Signifies that there were changes to the collection on the sync source that resulted in
    // our remote cursor getting killed.
    bool _lostNonResumableCursor = false;  // (X)
};

}  // namespace repl
}  // namespace mongo
