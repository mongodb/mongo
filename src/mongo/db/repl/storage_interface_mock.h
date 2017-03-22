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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace repl {

struct CollectionMockStats {
    bool initCalled = false;
    int insertCount = 0;
    bool commitCalled = false;
};

class CollectionBulkLoaderMock : public CollectionBulkLoader {
    MONGO_DISALLOW_COPYING(CollectionBulkLoaderMock);

public:
    CollectionBulkLoaderMock(CollectionMockStats* collStats) : stats(collStats){};
    virtual ~CollectionBulkLoaderMock() = default;
    virtual Status init(Collection* coll, const std::vector<BSONObj>& secondaryIndexSpecs) override;

    virtual Status insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                   const std::vector<BSONObj>::const_iterator end) override;
    virtual Status commit() override;

    std::string toString() const override {
        return toBSON().toString();
    };
    BSONObj toBSON() const override {
        return BSONObj();
    };

    CollectionMockStats* stats;

    // Override functions.
    stdx::function<Status(const std::vector<BSONObj>::const_iterator begin,
                          const std::vector<BSONObj>::const_iterator end)>
        insertDocsFn = [](const std::vector<BSONObj>::const_iterator,
                          const std::vector<BSONObj>::const_iterator) { return Status::OK(); };
    stdx::function<Status()> abortFn = []() { return Status::OK(); };
    stdx::function<Status()> commitFn = []() { return Status::OK(); };
    stdx::function<Status(Collection* coll, const std::vector<BSONObj>& secondaryIndexSpecs)>
        initFn = [](Collection*, const std::vector<BSONObj>&) { return Status::OK(); };
};

class StorageInterfaceMock : public StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterfaceMock);

public:
    // Used for testing.

    using CreateCollectionForBulkFn =
        stdx::function<StatusWith<std::unique_ptr<CollectionBulkLoader>>(
            const NamespaceString& nss,
            const CollectionOptions& options,
            const BSONObj idIndexSpec,
            const std::vector<BSONObj>& secondaryIndexSpecs)>;
    using InsertDocumentFn = stdx::function<Status(
        OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc)>;
    using InsertDocumentsFn = stdx::function<Status(
        OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs)>;
    using DropUserDatabasesFn = stdx::function<Status(OperationContext* opCtx)>;
    using CreateOplogFn =
        stdx::function<Status(OperationContext* opCtx, const NamespaceString& nss)>;
    using CreateCollectionFn = stdx::function<Status(
        OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options)>;
    using DropCollectionFn =
        stdx::function<Status(OperationContext* opCtx, const NamespaceString& nss)>;
    using FindDocumentsFn =
        stdx::function<StatusWith<std::vector<BSONObj>>(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        boost::optional<StringData> indexName,
                                                        ScanDirection scanDirection,
                                                        const BSONObj& startKey,
                                                        BoundInclusion boundInclusion,
                                                        std::size_t limit)>;
    using DeleteDocumentsFn =
        stdx::function<StatusWith<std::vector<BSONObj>>(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        boost::optional<StringData> indexName,
                                                        ScanDirection scanDirection,
                                                        const BSONObj& startKey,
                                                        BoundInclusion boundInclusion,
                                                        std::size_t limit)>;
    using IsAdminDbValidFn = stdx::function<Status(OperationContext* opCtx)>;

    StorageInterfaceMock() = default;

    void startup() override;
    void shutdown() override;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;
    void setInitialSyncFlag(OperationContext* opCtx) override;
    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) override;
    void setOplogDeleteFromPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogDeleteFromPoint(OperationContext* opCtx) override;
    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    OpTime getAppliedThrough(OperationContext* opCtx) override;

    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) override {
        return createCollectionForBulkFn(nss, options, idIndexSpec, secondaryIndexSpecs);
    };

    Status insertDocument(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& doc) override {
        return insertDocumentFn(opCtx, nss, doc);
    };

    Status insertDocuments(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const std::vector<BSONObj>& docs) override {
        return insertDocumentsFn(opCtx, nss, docs);
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

    StatusWith<StorageInterface::CollectionSize> getCollectionSize(
        OperationContext* opCtx, const NamespaceString& nss) override {
        return 0;
    }

    StatusWith<StorageInterface::CollectionCount> getCollectionCount(
        OperationContext* opCtx, const NamespaceString& nss) override {
        return 0;
    }

    Status isAdminDbValid(OperationContext* opCtx) override {
        return isAdminDbValidFn(opCtx);
    };


    // Testing functions.
    CreateCollectionForBulkFn createCollectionForBulkFn =
        [](const NamespaceString& nss,
           const CollectionOptions& options,
           const BSONObj idIndexSpec,
           const std::vector<BSONObj>&
               secondaryIndexSpecs) -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
        return Status{ErrorCodes::IllegalOperation, "CreateCollectionForBulkFn not implemented."};
    };
    InsertDocumentFn insertDocumentFn =
        [](OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
            return Status{ErrorCodes::IllegalOperation, "InsertDocumentFn not implemented."};
        };
    InsertDocumentsFn insertDocumentsFn =
        [](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
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
        return Status{ErrorCodes::IllegalOperation, "FindOneFn not implemented."};
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

private:
    mutable stdx::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable stdx::mutex _minValidBoundariesMutex;
    OpTime _appliedThrough;
    OpTime _minValid;
    Timestamp _oplogDeleteFromPoint;
};

}  // namespace repl
}  // namespace mongo
