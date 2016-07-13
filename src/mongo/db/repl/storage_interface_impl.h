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
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace mongo {
namespace repl {

class StorageInterfaceImpl : public StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterfaceImpl);

public:
    static const char kDefaultMinValidNamespace[];
    static const char kInitialSyncFlagFieldName[];
    static const char kBeginFieldName[];

    StorageInterfaceImpl();
    explicit StorageInterfaceImpl(const NamespaceString& minValidNss);
    virtual ~StorageInterfaceImpl();

    void startup() override;
    void shutdown() override;

    /**
     * Returns namespace of collection containing the minvalid boundaries and initial sync flag.
     */
    NamespaceString getMinValidNss() const;

    bool getInitialSyncFlag(OperationContext* txn) const override;

    void setInitialSyncFlag(OperationContext* txn) override;

    void clearInitialSyncFlag(OperationContext* txn) override;

    BatchBoundaries getMinValid(OperationContext* txn) const override;

    void setMinValid(OperationContext* ctx,
                     const OpTime& endOpTime,
                     const DurableRequirement durReq) override;

    void setMinValid(OperationContext* ctx, const BatchBoundaries& boundaries) override;

    /**
     *  Allocates a new TaskRunner for use by the passed in collection.
     */
    StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) override;

    Status insertDocument(OperationContext* txn,
                          const NamespaceString& nss,
                          const BSONObj& doc) override;

    Status insertDocuments(OperationContext* txn,
                           const NamespaceString& nss,
                           const std::vector<BSONObj>& docs) override;

    Status dropReplicatedDatabases(OperationContext* txn) override;

    Status createOplog(OperationContext* txn, const NamespaceString& nss) override;

    Status createCollection(OperationContext* txn,
                            const NamespaceString& nss,
                            const CollectionOptions& options) override;

    Status dropCollection(OperationContext* txn, const NamespaceString& nss) override;

    StatusWith<BSONObj> findOne(OperationContext* txn,
                                const NamespaceString& nss,
                                const BSONObj& indexKeyPattern,
                                ScanDirection scanDirection) override;

    StatusWith<BSONObj> deleteOne(OperationContext* txn,
                                  const NamespaceString& nss,
                                  const BSONObj& indexKeyPattern,
                                  ScanDirection scanDirection) override;

    Status isAdminDbValid(OperationContext* txn) override;

private:
    // One thread per collection/TaskRunner
    std::unique_ptr<OldThreadPool> _bulkLoaderThreads;
    const NamespaceString _minValidNss;

    // This mutex protects _runners vector.
    stdx::mutex _runnersMutex;

    // Each runner services a single collection and holds on to the OperationContext (and thread)
    // until it is done with the collection (CollectionBulkLoaderImpl::commit/abort is called).
    std::vector<std::pair<const NamespaceString, std::unique_ptr<TaskRunner>>> _runners;
};

}  // namespace repl
}  // namespace mongo
