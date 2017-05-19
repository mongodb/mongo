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
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"

namespace mongo {
namespace repl {

class StorageInterfaceImpl : public StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterfaceImpl);

public:
    static const char kDefaultMinValidNamespace[];
    static const char kInitialSyncFlagFieldName[];
    static const char kBeginFieldName[];
    static const char kOplogDeleteFromPointFieldName[];
    static const char kDefaultRollbackIdNamespace[];
    static const char kRollbackIdFieldName[];
    static const char kRollbackIdDocumentId[];

    StorageInterfaceImpl();
    explicit StorageInterfaceImpl(const NamespaceString& minValidNss);

    /**
     * Returns namespace of collection containing the minvalid boundaries and initial sync flag.
     */
    NamespaceString getMinValidNss() const;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;

    void setInitialSyncFlag(OperationContext* opCtx) override;

    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& endOpTime) override;
    StatusWith<int> getRollbackID(OperationContext* opCtx) override;
    Status initializeRollbackID(OperationContext* opCtx) override;
    Status incrementRollbackID(OperationContext* opCtx) override;
    void setOplogDeleteFromPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogDeleteFromPoint(OperationContext* opCtx) override;
    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    OpTime getAppliedThrough(OperationContext* opCtx) override;

    /**
     *  Allocates a new TaskRunner for use by the passed in collection.
     */
    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) override;

    Status insertDocument(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& doc) override;

    Status insertDocuments(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const std::vector<BSONObj>& docs) override;

    Status dropReplicatedDatabases(OperationContext* opCtx) override;

    Status createOplog(OperationContext* opCtx, const NamespaceString& nss) override;
    StatusWith<size_t> getOplogMaxSize(OperationContext* opCtx,
                                       const NamespaceString& nss) override;

    Status createCollection(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options) override;

    Status dropCollection(OperationContext* opCtx, const NamespaceString& nss) override;

    StatusWith<std::vector<BSONObj>> findDocuments(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   boost::optional<StringData> indexName,
                                                   ScanDirection scanDirection,
                                                   const BSONObj& startKey,
                                                   BoundInclusion boundInclusion,
                                                   std::size_t limit) override;

    StatusWith<std::vector<BSONObj>> deleteDocuments(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     boost::optional<StringData> indexName,
                                                     ScanDirection scanDirection,
                                                     const BSONObj& startKey,
                                                     BoundInclusion boundInclusion,
                                                     std::size_t limit) override;

    StatusWith<BSONObj> findSingleton(OperationContext* opCtx, const NamespaceString& nss) override;

    Status putSingleton(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& update) override;

    StatusWith<BSONObj> findById(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONElement& idKey) override;

    StatusWith<BSONObj> deleteById(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const BSONElement& idKey) override;

    Status upsertById(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const BSONElement& idKey,
                      const BSONObj& update) override;

    Status deleteByFilter(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& filter) override;

    StatusWith<StorageInterface::CollectionSize> getCollectionSize(
        OperationContext* opCtx, const NamespaceString& nss) override;

    StatusWith<StorageInterface::CollectionCount> getCollectionCount(
        OperationContext* opCtx, const NamespaceString& nss) override;

    /**
     * Checks that the "admin" database contains a supported version of the auth data schema.
     */
    Status isAdminDbValid(OperationContext* opCtx) override;

private:
    // Returns empty document if not present.
    BSONObj getMinValidDocument(OperationContext* opCtx) const;
    void updateMinValidDocument(OperationContext* opCtx, const BSONObj& updateSpec);

    const NamespaceString _minValidNss;
    const NamespaceString _rollbackIdNss;
};

}  // namespace repl
}  // namespace mongo
