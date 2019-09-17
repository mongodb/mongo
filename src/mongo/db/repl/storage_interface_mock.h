/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/platform/mutex.h"

namespace mongo {
namespace repl {

struct CollectionMockStats {
    bool initCalled = false;
    int insertCount = 0;
    bool commitCalled = false;
};

class CollectionBulkLoaderMock : public CollectionBulkLoader {
    CollectionBulkLoaderMock(const CollectionBulkLoaderMock&) = delete;
    CollectionBulkLoaderMock& operator=(const CollectionBulkLoaderMock&) = delete;

public:
    explicit CollectionBulkLoaderMock(std::shared_ptr<CollectionMockStats> collStats)
        : stats(std::move(collStats)){};
    virtual ~CollectionBulkLoaderMock() = default;
    Status init(const std::vector<BSONObj>& secondaryIndexSpecs) override;

    Status insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                           const std::vector<BSONObj>::const_iterator end) override;
    Status commit() override;

    std::string toString() const override {
        return toBSON().toString();
    };
    BSONObj toBSON() const override {
        return BSONObj();
    };

    std::shared_ptr<CollectionMockStats> stats;

    // Override functions.
    std::function<Status(std::vector<BSONObj>::const_iterator,
                         std::vector<BSONObj>::const_iterator)>
        insertDocsFn = [](const std::vector<BSONObj>::const_iterator,
                          const std::vector<BSONObj>::const_iterator) { return Status::OK(); };
    std::function<Status()> abortFn = []() { return Status::OK(); };
    std::function<Status()> commitFn = []() { return Status::OK(); };
};

class StorageInterfaceMock : public StorageInterface {
    StorageInterfaceMock(const StorageInterfaceMock&) = delete;
    StorageInterfaceMock& operator=(const StorageInterfaceMock&) = delete;

public:
    // Used for testing.

    using CreateCollectionForBulkFn = std::function<StatusWith<
        std::unique_ptr<CollectionBulkLoader>>(
        const NamespaceString&, const CollectionOptions&, BSONObj, const std::vector<BSONObj>&)>;
    using InsertDocumentFn = std::function<Status(
        OperationContext*, const NamespaceStringOrUUID&, const TimestampedBSONObj&, long long)>;
    using InsertDocumentsFn = std::function<Status(
        OperationContext*, const NamespaceStringOrUUID&, const std::vector<InsertStatement>&)>;
    using DropUserDatabasesFn = std::function<Status(OperationContext*)>;
    using CreateOplogFn = std::function<Status(OperationContext*, const NamespaceString&)>;
    using CreateCollectionFn =
        std::function<Status(OperationContext*, const NamespaceString&, const CollectionOptions&)>;
    using TruncateCollectionFn =
        std::function<Status(OperationContext*, const NamespaceString& nss)>;
    using DropCollectionFn = std::function<Status(OperationContext*, const NamespaceString& nss)>;
    using FindDocumentsFn =
        std::function<StatusWith<std::vector<BSONObj>>(OperationContext*,
                                                       const NamespaceString&,
                                                       boost::optional<StringData>,
                                                       ScanDirection,
                                                       const BSONObj&,
                                                       BoundInclusion,
                                                       std::size_t)>;
    using DeleteDocumentsFn =
        std::function<StatusWith<std::vector<BSONObj>>(OperationContext*,
                                                       const NamespaceString&,
                                                       boost::optional<StringData>,
                                                       ScanDirection,
                                                       const BSONObj&,
                                                       BoundInclusion,
                                                       std::size_t)>;
    using IsAdminDbValidFn = std::function<Status(OperationContext*)>;
    using GetCollectionUUIDFn = std::function<StatusWith<OptionalCollectionUUID>(
        OperationContext*, const NamespaceString&)>;

    StorageInterfaceMock() = default;

    StatusWith<int> getRollbackID(OperationContext* opCtx) override;
    StatusWith<int> initializeRollbackID(OperationContext* opCtx) override;
    StatusWith<int> incrementRollbackID(OperationContext* opCtx) override;

    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) override {
        return createCollectionForBulkFn(nss, options, idIndexSpec, secondaryIndexSpecs);
    };

    Status insertDocument(OperationContext* opCtx,
                          const NamespaceStringOrUUID& nsOrUUID,
                          const TimestampedBSONObj& doc,
                          long long term) override {
        return insertDocumentFn(opCtx, nsOrUUID, doc, term);
    };

    Status insertDocuments(OperationContext* opCtx,
                           const NamespaceStringOrUUID& nsOrUUID,
                           const std::vector<InsertStatement>& docs) override {
        return insertDocumentsFn(opCtx, nsOrUUID, docs);
    }

    Status dropReplicatedDatabases(OperationContext* opCtx) override {
        return dropUserDBsFn(opCtx);
    };

    Status createOplog(OperationContext* opCtx, const NamespaceString& nss) override {
        return createOplogFn(opCtx, nss);
    };

    StatusWith<size_t> getOplogMaxSize(OperationContext* opCtx,
                                       const NamespaceString& nss) override {
        return 1024 * 1024 * 1024;
    }

    Status createCollection(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options) override {
        return createCollFn(opCtx, nss, options);
    }

    Status dropCollection(OperationContext* opCtx, const NamespaceString& nss) override {
        return dropCollFn(opCtx, nss);
    };

    Status truncateCollection(OperationContext* opCtx, const NamespaceString& nss) override {
        return truncateCollFn(opCtx, nss);
    }

    Status renameCollection(OperationContext* opCtx,
                            const NamespaceString& fromNS,
                            const NamespaceString& toNS,
                            bool stayTemp) override {

        return Status{ErrorCodes::IllegalOperation, "renameCollection not implemented."};
    }

    Status setIndexIsMultikey(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const std::string& indexName,
                              const MultikeyPaths& paths,
                              Timestamp ts) override {

        return Status{ErrorCodes::IllegalOperation, "setIndexIsMultikey not implemented."};
    }

    StatusWith<std::vector<BSONObj>> findDocuments(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   boost::optional<StringData> indexName,
                                                   ScanDirection scanDirection,
                                                   const BSONObj& startKey,
                                                   BoundInclusion boundInclusion,
                                                   std::size_t limit) override {
        return findDocumentsFn(
            opCtx, nss, indexName, scanDirection, startKey, boundInclusion, limit);
    }

    StatusWith<std::vector<BSONObj>> deleteDocuments(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     boost::optional<StringData> indexName,
                                                     ScanDirection scanDirection,
                                                     const BSONObj& startKey,
                                                     BoundInclusion boundInclusion,
                                                     std::size_t limit) override {
        return deleteDocumentsFn(
            opCtx, nss, indexName, scanDirection, startKey, boundInclusion, limit);
    }

    StatusWith<BSONObj> findSingleton(OperationContext* opCtx,
                                      const NamespaceString& nss) override {
        return Status{ErrorCodes::IllegalOperation, "findSingleton not implemented."};
    }

    Status putSingleton(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const TimestampedBSONObj& update) override {
        return Status{ErrorCodes::IllegalOperation, "putSingleton not implemented."};
    }

    Status updateSingleton(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& query,
                           const TimestampedBSONObj& update) override {
        return Status{ErrorCodes::IllegalOperation, "updateSingleton not implemented."};
    }

    StatusWith<BSONObj> findById(OperationContext* opCtx,
                                 const NamespaceStringOrUUID&,
                                 const BSONElement& idKey) override {
        return Status{ErrorCodes::IllegalOperation, "findById not implemented."};
    }

    StatusWith<BSONObj> deleteById(OperationContext* opCtx,
                                   const NamespaceStringOrUUID&,
                                   const BSONElement& idKey) override {
        return Status{ErrorCodes::IllegalOperation, "deleteById not implemented."};
    }

    Status upsertById(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      const BSONElement& idKey,
                      const BSONObj& update) override {
        return Status{ErrorCodes::IllegalOperation, "upsertById not implemented."};
    }

    Status deleteByFilter(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& filter) override {
        return Status{ErrorCodes::IllegalOperation, "deleteByFilter not implemented."};
    }

    StatusWith<StorageInterface::CollectionSize> getCollectionSize(
        OperationContext* opCtx, const NamespaceString& nss) override {
        return 0;
    }

    StatusWith<StorageInterface::CollectionCount> getCollectionCount(
        OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) override {
        return 0;
    }

    Status setCollectionCount(OperationContext* opCtx,
                              const NamespaceStringOrUUID& nsOrUUID,
                              long long newCount) override {
        return Status{ErrorCodes::IllegalOperation, "setCollectionCount not implemented."};
    }

    StatusWith<OptionalCollectionUUID> getCollectionUUID(OperationContext* opCtx,
                                                         const NamespaceString& nss) override {
        return getCollectionUUIDFn(opCtx, nss);
    }

    void setStableTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override;

    void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override;

    Timestamp getStableTimestamp() const;

    Timestamp getInitialDataTimestamp() const;

    StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) override {
        return Status{ErrorCodes::IllegalOperation, "recoverToStableTimestamp not implemented."};
    }

    bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const override {
        return false;
    }

    bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        return false;
    }

    void initializeStorageControlsForReplication(ServiceContext* serviceCtx) const override {}

    boost::optional<Timestamp> getRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        return boost::none;
    }

    Timestamp getAllDurableTimestamp(ServiceContext* serviceCtx) const override;

    Timestamp getOldestOpenReadTimestamp(ServiceContext* serviceCtx) const override;

    bool supportsDocLocking(ServiceContext* serviceCtx) const override;

    Status isAdminDbValid(OperationContext* opCtx) override {
        return isAdminDbValidFn(opCtx);
    };

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 bool primaryOnly) override {
        return;
    }

    void oplogDiskLocRegister(OperationContext* opCtx,
                              const Timestamp& ts,
                              bool orderedCommit) override {
        return;
    }

    boost::optional<Timestamp> getLastStableRecoveryTimestamp(
        ServiceContext* serviceCtx) const override {
        return boost::none;
    }

    Timestamp getPointInTimeReadTimestamp(OperationContext* opCtx) const override {
        return {};
    }

    // Testing functions.
    CreateCollectionForBulkFn createCollectionForBulkFn =
        [](const NamespaceString& nss,
           const CollectionOptions& options,
           const BSONObj idIndexSpec,
           const std::vector<BSONObj>& secondaryIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
        return Status{ErrorCodes::IllegalOperation, "CreateCollectionForBulkFn not implemented."};
    };
    InsertDocumentFn insertDocumentFn = [](OperationContext* opCtx,
                                           const NamespaceStringOrUUID& nsOrUUID,
                                           const TimestampedBSONObj& doc,
                                           long long term) {
        return Status{ErrorCodes::IllegalOperation, "InsertDocumentFn not implemented."};
    };
    InsertDocumentsFn insertDocumentsFn = [](OperationContext* opCtx,
                                             const NamespaceStringOrUUID& nsOrUUID,
                                             const std::vector<InsertStatement>& docs) {
        return Status{ErrorCodes::IllegalOperation, "InsertDocumentsFn not implemented."};
    };
    DropUserDatabasesFn dropUserDBsFn = [](OperationContext* opCtx) {
        return Status{ErrorCodes::IllegalOperation, "DropUserDatabasesFn not implemented."};
    };
    CreateOplogFn createOplogFn = [](OperationContext* opCtx, const NamespaceString& nss) {
        return Status{ErrorCodes::IllegalOperation, "CreateOplogFn not implemented."};
    };
    CreateCollectionFn createCollFn =
        [](OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options) {
            return Status{ErrorCodes::IllegalOperation, "CreateCollectionFn not implemented."};
        };
    TruncateCollectionFn truncateCollFn = [](OperationContext* opCtx, const NamespaceString& nss) {
        return Status{ErrorCodes::IllegalOperation, "TruncateCollectionFn not implemented."};
    };
    DropCollectionFn dropCollFn = [](OperationContext* opCtx, const NamespaceString& nss) {
        return Status{ErrorCodes::IllegalOperation, "DropCollectionFn not implemented."};
    };
    FindDocumentsFn findDocumentsFn = [](OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         boost::optional<StringData> indexName,
                                         ScanDirection scanDirection,
                                         const BSONObj& startKey,
                                         BoundInclusion boundInclusion,
                                         std::size_t limit) {
        return Status{ErrorCodes::IllegalOperation, "FindDocumentsFn not implemented."};
    };
    DeleteDocumentsFn deleteDocumentsFn = [](OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             boost::optional<StringData> indexName,
                                             ScanDirection scanDirection,
                                             const BSONObj& startKey,
                                             BoundInclusion boundInclusion,
                                             std::size_t limit) {
        return Status{ErrorCodes::IllegalOperation, "DeleteOneFn not implemented."};
    };
    IsAdminDbValidFn isAdminDbValidFn = [](OperationContext*) {
        return Status{ErrorCodes::IllegalOperation, "IsAdminDbValidFn not implemented."};
    };
    GetCollectionUUIDFn getCollectionUUIDFn =
        [](OperationContext* opCtx,
           const NamespaceString& nss) -> StatusWith<OptionalCollectionUUID> {
        return Status{ErrorCodes::IllegalOperation, "GetCollectionUUIDFn not implemented."};
    };

    bool supportsDocLockingBool = false;
    Timestamp allDurableTimestamp = Timestamp::min();
    Timestamp oldestOpenReadTimestamp = Timestamp::min();

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("StorageInterfaceMock::_mutex");
    int _rbid;
    bool _rbidInitialized = false;
    Timestamp _stableTimestamp = Timestamp::min();
    Timestamp _initialDataTimestamp = Timestamp::min();
    OptionalCollectionUUID _uuid;
    bool _schemaUpgraded;
};

}  // namespace repl
}  // namespace mongo
