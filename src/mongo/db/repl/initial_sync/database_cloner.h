// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/collection_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace mongo {
namespace repl {

class DatabaseClonerTest;

struct InitialSyncSummaryStats;

class [[MONGO_MOD_PUBLIC]] DatabaseCloner final : public InitialSyncBaseCloner {
public:
    struct Stats {
        DatabaseName dbname;
        Date_t start;
        Date_t end;
        size_t collections{0};
        size_t clonedCollections{0};
        std::vector<CollectionCloner::Stats> collectionStats;

        void append(BSONObjBuilder* builder) const;
    };

    DatabaseCloner(const DatabaseName& dbName,
                   InitialSyncSharedData* sharedData,
                   const HostAndPort& source,
                   DBClientConnection* client,
                   StorageInterface* storageInterface,
                   ThreadPool* dbPool,
                   std::shared_ptr<InitialSyncSummaryStats> summaryStats);

    ~DatabaseCloner() override = default;

    Stats getStats() const;

    std::string toString() const;

    static CollectionOptions parseCollectionOptions(const BSONObj& element);

protected:
    ClonerStages getStages() final;

    bool isMyFailPoint(const BSONObj& data) const final;

private:
    friend class DatabaseClonerTest;

    /**
     * Stage function that retrieves collection information from the sync source.
     */
    AfterStageBehavior listCollectionsStage();

    /**
     * The preStage sets the start time in _stats.
     */
    void preStage() final;

    /**
     * The postStage creates and runs the individual CollectionCloners on each database found on
     * the sync source, and sets the end time in _stats when done.
     */
    void postStage() final;

    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        return toStringForLogging(_dbName) + " db: { " + stage->getName() + ": 1 } ";
    }

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    // (MX) Write access with mutex from main flow of control, read access with mutex from other
    //      threads, read access allowed from main flow without mutex.
    const DatabaseName _dbName;                         // (R)
    ClonerStage<DatabaseCloner> _listCollectionsStage;  // (R)
    std::vector<std::tuple<NamespaceString, CollectionOptions, bool /*recordIdsReplicated*/>>
        _collections;                                            // (X)
    std::unique_ptr<CollectionCloner> _currentCollectionCloner;  // (MX)
    Stats _stats;                                                // (MX)
    std::shared_ptr<InitialSyncSummaryStats> _summaryStats;      // (R)
};

}  // namespace repl
}  // namespace mongo
