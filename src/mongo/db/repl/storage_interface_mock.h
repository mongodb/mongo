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
    virtual Status init(OperationContext* txn,
                        Collection* coll,
                        const std::vector<BSONObj>& secondaryIndexSpecs) override;

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
    stdx::function<Status(
        OperationContext* txn, Collection* coll, const std::vector<BSONObj>& secondaryIndexSpecs)>
        initFn = [](OperationContext*, Collection*, const std::vector<BSONObj>&) {
            return Status::OK();
        };
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
        OperationContext* txn, const NamespaceString& nss, const BSONObj& doc)>;
    using InsertDocumentsFn = stdx::function<Status(
        OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& docs)>;
    using DropUserDatabasesFn = stdx::function<Status(OperationContext* txn)>;
    using CreateOplogFn = stdx::function<Status(OperationContext* txn, const NamespaceString& nss)>;
    using CreateCollectionFn = stdx::function<Status(
        OperationContext* txn, const NamespaceString& nss, const CollectionOptions& options)>;
    using DropCollectionFn =
        stdx::function<Status(OperationContext* txn, const NamespaceString& nss)>;
    using FindOneFn = stdx::function<StatusWith<BSONObj>(OperationContext* txn,
                                                         const NamespaceString& nss,
                                                         const BSONObj& indexKeyPattern,
                                                         ScanDirection scanDirection)>;
    using DeleteOneFn = stdx::function<StatusWith<BSONObj>(OperationContext* txn,
                                                           const NamespaceString& nss,
                                                           const BSONObj& indexKeyPattern,
                                                           ScanDirection scanDirection)>;

    StorageInterfaceMock() = default;

    void startup() override;
    void shutdown() override;

    bool getInitialSyncFlag(OperationContext* txn) const override;
    void setInitialSyncFlag(OperationContext* txn) override;
    void clearInitialSyncFlag(OperationContext* txn) override;

    BatchBoundaries getMinValid(OperationContext* txn) const override;
    void setMinValid(OperationContext* txn,
                     const OpTime& endOpTime,
                     const DurableRequirement durReq) override;
    void setMinValid(OperationContext* txn, const BatchBoundaries& boundaries) override;

    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) override {
        return createCollectionForBulkFn(nss, options, idIndexSpec, secondaryIndexSpecs);
    };

    Status insertDocument(OperationContext* txn,
                          const NamespaceString& nss,
                          const BSONObj& doc) override {
        return insertDocumentFn(txn, nss, doc);
    };

    Status insertDocuments(OperationContext* txn,
                           const NamespaceString& nss,
                           const std::vector<BSONObj>& docs) override {
        return insertDocumentsFn(txn, nss, docs);
    }

    Status dropReplicatedDatabases(OperationContext* txn) override {
        return dropUserDBsFn(txn);
    };

    Status createOplog(OperationContext* txn, const NamespaceString& nss) override {
        return createOplogFn(txn, nss);
    };

    Status createCollection(OperationContext* txn,
                            const NamespaceString& nss,
                            const CollectionOptions& options) override {
        return createCollFn(txn, nss, options);
    }

    Status dropCollection(OperationContext* txn, const NamespaceString& nss) override {
        return dropCollFn(txn, nss);
    };

    StatusWith<BSONObj> findOne(OperationContext* txn,
                                const NamespaceString& nss,
                                const BSONObj& indexKeyPattern,
                                ScanDirection scanDirection) override {
        return findOneFn(txn, nss, indexKeyPattern, scanDirection);
    }

    StatusWith<BSONObj> deleteOne(OperationContext* txn,
                                  const NamespaceString& nss,
                                  const BSONObj& indexKeyPattern,
                                  ScanDirection scanDirection) override {
        return deleteOneFn(txn, nss, indexKeyPattern, scanDirection);
    }

    Status isAdminDbValid(OperationContext* txn) override {
        return Status::OK();
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
        [](OperationContext* txn, const NamespaceString& nss, const BSONObj& doc) {
            return Status{ErrorCodes::IllegalOperation, "InsertDocumentFn not implemented."};
        };
    InsertDocumentsFn insertDocumentsFn =
        [](OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            return Status{ErrorCodes::IllegalOperation, "InsertDocumentsFn not implemented."};
        };
    DropUserDatabasesFn dropUserDBsFn = [](OperationContext* txn) {
        return Status{ErrorCodes::IllegalOperation, "DropUserDatabasesFn not implemented."};
    };
    CreateOplogFn createOplogFn = [](OperationContext* txn, const NamespaceString& nss) {
        return Status{ErrorCodes::IllegalOperation, "CreateOplogFn not implemented."};
    };
    CreateCollectionFn createCollFn =
        [](OperationContext* txn, const NamespaceString& nss, const CollectionOptions& options) {
            return Status{ErrorCodes::IllegalOperation, "CreateCollectionFn not implemented."};
        };
    DropCollectionFn dropCollFn = [](OperationContext* txn, const NamespaceString& nss) {
        return Status{ErrorCodes::IllegalOperation, "DropCollectionFn not implemented."};
    };
    FindOneFn findOneFn = [](OperationContext* txn,
                             const NamespaceString& nss,
                             const BSONObj& indexKeyPattern,
                             ScanDirection scanDirection) {
        return Status{ErrorCodes::IllegalOperation, "FindOneFn not implemented."};
    };
    DeleteOneFn deleteOneFn = [](OperationContext* txn,
                                 const NamespaceString& nss,
                                 const BSONObj& indexKeyPattern,
                                 ScanDirection scanDirection) {
        return Status{ErrorCodes::IllegalOperation, "DeleteOneFn not implemented."};
    };

private:
    mutable stdx::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable stdx::mutex _minValidBoundariesMutex;
    BatchBoundaries _minValidBoundaries = {OpTime(), OpTime()};
};

}  // namespace repl
}  // namespace mongo
