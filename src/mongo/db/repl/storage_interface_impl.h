// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

class [[MONGO_MOD_OPEN]] StorageInterfaceImpl : public StorageInterface {
    StorageInterfaceImpl(const StorageInterfaceImpl&) = delete;
    StorageInterfaceImpl& operator=(const StorageInterfaceImpl&) = delete;

public:
    static const char kRollbackIdFieldName[];
    static const char kRollbackIdDocumentId[];

    StorageInterfaceImpl();

    StatusWith<int> getRollbackID(OperationContext* opCtx) override;
    StatusWith<int> initializeRollbackID(OperationContext* opCtx) override;
    StatusWith<int> incrementRollbackID(OperationContext* opCtx) override;

    /**
     *  Allocates a new TaskRunner for use by the passed in collection.
     */
    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs,
        boost::optional<bool> recordIdsReplicated = boost::none) override;

    Status insertDocument(OperationContext* opCtx,
                          const NamespaceStringOrUUID& nsOrUUID,
                          const TimestampedBSONObj& doc,
                          long long term) override;

    Status insertDocuments(OperationContext* opCtx,
                           const NamespaceStringOrUUID& nsOrUUID,
                           const std::vector<InsertStatement>& docs) override;

    Status dropReplicatedDatabases(OperationContext* opCtx) override;

    Status createOplog(OperationContext* opCtx, const NamespaceString& nss) override;
    StatusWith<size_t> getOplogMaxSize(OperationContext* opCtx) override;

    Status createCollection(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options,
                            bool createIdIndex = true,
                            const BSONObj& idIndexSpec = BSONObj()) override;

    Status createIndexesOnEmptyCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const std::vector<BSONObj>& secondaryIndexSpecs) override;

    Status dropCollection(OperationContext* opCtx, const NamespaceString& nss) override;

    Status dropCollectionsWithPrefix(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const std::string& collectionNamePrefix) override;

    Status truncateCollection(OperationContext* opCtx, const NamespaceString& nss) override;

    Status renameCollection(OperationContext* opCtx,
                            const NamespaceString& fromNS,
                            const NamespaceString& toNS,
                            bool stayTemp) override;

    Status setIndexIsMultikey(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const UUID& collectionUUID,
                              const std::string& indexName,
                              const KeyStringSet& multikeyMetadataKeys,
                              const MultikeyPaths& paths,
                              Timestamp ts) override;

    StatusWith<std::vector<BSONObj>> findDocuments(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   boost::optional<std::string_view> indexName,
                                                   ScanDirection scanDirection,
                                                   const BSONObj& startKey,
                                                   BoundInclusion boundInclusion,
                                                   std::size_t limit) override;

    StatusWith<std::vector<BSONObj>> deleteDocuments(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     boost::optional<std::string_view> indexName,
                                                     ScanDirection scanDirection,
                                                     const BSONObj& startKey,
                                                     BoundInclusion boundInclusion,
                                                     std::size_t limit) override;

    StatusWith<BSONObj> findSingleton(OperationContext* opCtx, const NamespaceString& nss) override;

    Status putSingleton(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const TimestampedBSONObj& update) override;

    Status putSingleton(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& query,
                        const TimestampedBSONObj& update) override;

    Status updateSingleton(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& query,
                           const TimestampedBSONObj& update) override;

    Status updateDocuments(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const BSONObj& query,
        const TimestampedBSONObj& update,
        const boost::optional<std::vector<BSONObj>>& arrayFilters = boost::none) override;

    StatusWith<BSONObj> findById(OperationContext* opCtx,
                                 const NamespaceStringOrUUID& nsOrUUID,
                                 const BSONElement& idKey) override;

    StatusWith<BSONObj> deleteById(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID,
                                   const BSONElement& idKey) override;

    Status upsertById(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      const BSONElement& idKey,
                      const BSONObj& update) override;

    Status deleteByFilter(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& filter) override;

    boost::optional<OpTimeAndWallTime> findOplogOpTimeLessThanOrEqualToTimestamp(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) override;

    boost::optional<OpTimeAndWallTime> findOplogOpTimeLessThanOrEqualToTimestampRetryOnWCE(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) override;

    Timestamp getEarliestOplogTimestamp(OperationContext* opCtx) override;
    Timestamp getLatestOplogTimestamp(OperationContext* opCtx) override;
    Timestamp getOldestTimestamp(ServiceContext* serviceCtx) override;

    StatusWith<StorageInterface::CollectionSize> getCollectionSize(
        OperationContext* opCtx, const NamespaceString& nss) override;

    StatusWith<StorageInterface::CollectionCount> getCollectionCount(
        OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) override;

    Status setCollectionCount(OperationContext* opCtx,
                              const NamespaceStringOrUUID& nsOrUUID,
                              long long newCount) override;

    StatusWith<UUID> getCollectionUUID(OperationContext* opCtx,
                                       const NamespaceString& nss) override;

    void setStableTimestamp(ServiceContext* serviceCtx,
                            Timestamp snapshotName,
                            bool force = false) override;

    void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override;

    Timestamp getInitialDataTimestamp(ServiceContext* serviceCtx) const override;

    Timestamp recoverToStableTimestamp(OperationContext* opCtx) override;

    bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const override;

    bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const override;

    boost::optional<Timestamp> getRecoveryTimestamp(ServiceContext* serviceCtx) const override;

    Timestamp getAllDurableTimestamp(ServiceContext* serviceCtx) const override;

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 bool primaryOnly) override;
    void oplogDiskLocRegister(OperationContext* opCtx,
                              const Timestamp& ts,
                              bool orderedCommit) override;

    boost::optional<Timestamp> getLastStableRecoveryTimestamp(
        ServiceContext* serviceCtx) const override;

    Timestamp getPointInTimeReadTimestamp(OperationContext* opCtx) const override;

    void setPinnedOplogTimestamp(OperationContext* opCtx,
                                 const Timestamp& pinnedTimestamp) const override;

private:
    const NamespaceString _rollbackIdNss;
};

}  // namespace repl
}  // namespace mongo
