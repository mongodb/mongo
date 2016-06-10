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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/storage_interface_impl.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rs_initialsync.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
namespace mongo {
namespace repl {

const char StorageInterfaceImpl::kDefaultMinValidNamespace[] = "local.replset.minvalid";
const char StorageInterfaceImpl::kInitialSyncFlagFieldName[] = "doingInitialSync";
const char StorageInterfaceImpl::kBeginFieldName[] = "begin";

namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(dataReplicatorInitialSyncInserterThreads, int, 4);

const BSONObj kInitialSyncFlag(BSON(StorageInterfaceImpl::kInitialSyncFlagFieldName << true));
}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : StorageInterfaceImpl(NamespaceString(StorageInterfaceImpl::kDefaultMinValidNamespace)) {}

StorageInterfaceImpl::StorageInterfaceImpl(const NamespaceString& minValidNss)
    : _minValidNss(minValidNss) {}

StorageInterfaceImpl::~StorageInterfaceImpl() {
    DESTRUCTOR_GUARD(shutdown(););
}

void StorageInterfaceImpl::startup() {
    _bulkLoaderThreads.reset(
        new OldThreadPool{dataReplicatorInitialSyncInserterThreads, "InitialSyncInserters-"});
};

void StorageInterfaceImpl::shutdown() {
    if (_bulkLoaderThreads) {
        _bulkLoaderThreads->join();
        _bulkLoaderThreads.reset();
    }
}

NamespaceString StorageInterfaceImpl::getMinValidNss() const {
    return _minValidNss;
}

bool StorageInterfaceImpl::getInitialSyncFlag(OperationContext* txn) const {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), _minValidNss.ns(), MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(txn, _minValidNss.ns().c_str(), mv);

        if (found) {
            const auto flag = mv[kInitialSyncFlagFieldName].trueValue();
            LOG(3) << "return initial flag value of " << flag;
            return flag;
        }
        LOG(3) << "return initial flag value of false";
        return false;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::getInitialSyncFlag", _minValidNss.ns());

    MONGO_UNREACHABLE;
}

void StorageInterfaceImpl::setInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn, _minValidNss.ns().c_str(), BSON("$set" << kInitialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setInitialSyncFlag", _minValidNss.ns());

    txn->recoveryUnit()->waitUntilDurable();
    LOG(3) << "setting initial sync flag";
}

void StorageInterfaceImpl::clearInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        // TODO: Investigate correctness of taking MODE_IX for DB/Collection locks
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn, _minValidNss.ns().c_str(), BSON("$unset" << kInitialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::clearInitialSyncFlag", _minValidNss.ns());

    auto replCoord = repl::ReplicationCoordinator::get(txn);
    if (getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()) {
        OpTime time = replCoord->getMyLastAppliedOpTime();
        txn->recoveryUnit()->waitUntilDurable();
        replCoord->setMyLastDurableOpTime(time);
    }
    LOG(3) << "clearing initial sync flag";
}

BatchBoundaries StorageInterfaceImpl::getMinValid(OperationContext* txn) const {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), _minValidNss.ns(), MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(txn, _minValidNss.ns().c_str(), mv);
        if (found) {
            auto status = OpTime::parseFromOplogEntry(mv.getObjectField(kBeginFieldName));
            OpTime start(status.isOK() ? status.getValue() : OpTime{});
            const auto opTimeStatus = OpTime::parseFromOplogEntry(mv);
            // If any of the keys (fields) are missing from the minvalid document, we return
            // empty.
            if (opTimeStatus == ErrorCodes::NoSuchKey) {
                return BatchBoundaries{{}, {}};
            }

            if (!opTimeStatus.isOK()) {
                error() << "Error parsing minvalid entry: " << mv
                        << ", with status:" << opTimeStatus.getStatus();
            }
            OpTime end(fassertStatusOK(40052, opTimeStatus));
            LOG(3) << "returning minvalid: " << start.toString() << "(" << start.toBSON() << ") -> "
                   << end.toString() << "(" << end.toBSON() << ")";

            return BatchBoundaries(start, end);
        }
        LOG(3) << "returning empty minvalid";
        return BatchBoundaries{{}, {}};
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::getMinValid", _minValidNss.ns());
}

void StorageInterfaceImpl::setMinValid(OperationContext* txn,
                                       const OpTime& endOpTime,
                                       const DurableRequirement durReq) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(
            txn,
            _minValidNss.ns().c_str(),
            BSON("$set" << BSON("ts" << endOpTime.getTimestamp() << "t" << endOpTime.getTerm())
                        << "$unset"
                        << BSON(kBeginFieldName << 1)));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setMinValid", _minValidNss.ns());

    if (durReq == DurableRequirement::Strong) {
        txn->recoveryUnit()->waitUntilDurable();
    }
    LOG(3) << "setting minvalid: " << endOpTime.toString() << "(" << endOpTime.toBSON() << ")";
}

void StorageInterfaceImpl::setMinValid(OperationContext* txn, const BatchBoundaries& boundaries) {
    const OpTime& start(boundaries.start);
    const OpTime& end(boundaries.end);
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn,
                              _minValidNss.ns().c_str(),
                              BSON("$set" << BSON("ts" << end.getTimestamp() << "t" << end.getTerm()
                                                       << kBeginFieldName
                                                       << start.toBSON())));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setMinValid", _minValidNss.ns());
    // NOTE: No need to ensure durability here since starting a batch isn't a problem unless
    // writes happen after, in which case this marker (minvalid) will be written already.
    LOG(3) << "setting minvalid: " << boundaries.start.toString() << "("
           << boundaries.start.toBSON() << ") -> " << boundaries.end.toString() << "("
           << boundaries.end.toBSON() << ")";
}

StatusWith<OpTime> StorageInterfaceImpl::writeOpsToOplog(
    OperationContext* txn, const NamespaceString& nss, const MultiApplier::Operations& operations) {
    if (operations.empty()) {
        return {ErrorCodes::EmptyArrayOperation,
                "unable to write operations to oplog - no operations provided"};
    }

    std::vector<BSONObj> ops(operations.size());
    auto toBSON = [](const OplogEntry& entry) { return entry.raw; };
    std::transform(operations.begin(), operations.end(), ops.begin(), toBSON);

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetCollection collectionWriteGuard(txn, nss, MODE_X);
        auto collection = collectionWriteGuard.getCollection();
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "unable to write operations to oplog at " << nss.ns()
                                  << ": collection not found. Did you drop it?"};
        }

        WriteUnitOfWork wunit(txn);
        OpDebug* const nullOpDebug = nullptr;
        auto status = collection->insertDocuments(txn, ops.begin(), ops.end(), nullOpDebug, false);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "writeOpsToOplog", nss.ns());

    return operations.back().getOpTime();
}

StatusWith<std::unique_ptr<CollectionBulkLoader>>
StorageInterfaceImpl::createCollectionForBulkLoading(
    const NamespaceString& nss,
    const CollectionOptions& options,
    const BSONObj idIndexSpec,
    const std::vector<BSONObj>& secondaryIndexSpecs) {

    UniqueLock lk(_runnersMutex);
    // Check to make sure we don't already have a runner.
    for (auto&& item : _runners) {
        if (item.first == nss) {
            return {ErrorCodes::IllegalOperation,
                    str::stream() << "There is already an active collection cloner for: "
                                  << nss.ns()};
        }
    }
    // Create the runner, and schedule the collection creation.
    _runners.emplace_back(
        std::make_pair(nss, stdx::make_unique<TaskRunner>(_bulkLoaderThreads.get())));
    auto&& inserter = _runners.back();
    TaskRunner* runner = inserter.second.get();
    lk.unlock();

    // Setup cond_var for signalling when done.
    std::unique_ptr<CollectionBulkLoader> loaderToReturn;

    auto status = runner->runSynchronousTask([&](OperationContext* txn) -> Status {
        // We are not replicating nor validating these writes.
        txn->setReplicatedWrites(false);
        DisableDocumentValidation validationDisabler(txn);

        // Retry if WCE.
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            // Get locks and create the collection.
            ScopedTransaction transaction(txn, MODE_IX);
            auto db = stdx::make_unique<AutoGetOrCreateDb>(txn, nss.db(), MODE_IX);
            auto coll = stdx::make_unique<AutoGetCollection>(txn, nss, MODE_X);
            Collection* collection = coll->getCollection();

            if (collection) {
                return {ErrorCodes::NamespaceExists, "Collection already exists."};
            }

            // Create the collection.
            WriteUnitOfWork wunit(txn);
            collection = db->getDb()->createCollection(txn, nss.ns(), options, false);
            invariant(collection);
            wunit.commit();
            coll = stdx::make_unique<AutoGetCollection>(txn, nss, MODE_IX);

            // Move locks into loader, so it now controls their lifetime.
            auto loader = stdx::make_unique<CollectionBulkLoaderImpl>(
                txn, runner, collection, idIndexSpec, std::move(db), std::move(coll));
            invariant(collection);
            auto status = loader->init(txn, collection, secondaryIndexSpecs);
            if (!status.isOK()) {
                return status;
            }

            // Move the loader into the StatusWith.
            loaderToReturn = std::move(loader);
            return Status::OK();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "beginCollectionClone", nss.ns());
        MONGO_UNREACHABLE;
    });

    if (!status.isOK()) {
        return status;
    }

    return std::move(loaderToReturn);
}


Status StorageInterfaceImpl::insertDocument(OperationContext* txn,
                                            const NamespaceString& nss,
                                            const BSONObj& doc) {
    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetOrCreateDb autoDB(txn, nss.db(), MODE_IX);
    AutoGetCollection autoColl(txn, nss, MODE_IX);
    if (!autoColl.getCollection()) {
        return {ErrorCodes::NamespaceNotFound,
                "The collection must exist before inserting documents."};
    }
    WriteUnitOfWork wunit(txn);
    const auto status =
        autoColl.getCollection()->insertDocument(txn, doc, nullptr /** OpDebug **/, false, false);
    if (status.isOK()) {
        wunit.commit();
    }
    return status;
}

StatusWith<OpTime> StorageInterfaceImpl::insertOplogDocuments(OperationContext* txn,
                                                              const NamespaceString& nss,
                                                              const std::vector<BSONObj>& ops) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetOrCreateDb autoDB(txn, nss.db(), MODE_IX);
        AutoGetCollection autoColl(txn, nss, MODE_IX);
        if (!autoColl.getCollection()) {
            return {
                ErrorCodes::NamespaceNotFound,
                str::stream() << "The oplog collection must exist before inserting documents, ns:"
                              << nss.ns()};
        }
        if (!autoColl.getCollection()->isCapped()) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The oplog collection must be capped, ns:" << nss.ns()};
        }

        WriteUnitOfWork wunit(txn);
        const auto lastOpTime = repl::writeOpsToOplog(txn, nss.ns(), ops);
        wunit.commit();
        return lastOpTime;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::insertOplogDocuments", nss.ns());
}

Status StorageInterfaceImpl::dropReplicatedDatabases(OperationContext* txn) {
    dropAllDatabasesExceptLocal(txn);
    return Status::OK();
}

Status StorageInterfaceImpl::createOplog(OperationContext* txn, const NamespaceString& nss) {
    mongo::repl::createOplog(txn, nss.ns(), true);
    return Status::OK();
}

Status StorageInterfaceImpl::dropCollection(OperationContext* txn, const NamespaceString& nss) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetOrCreateDb autoDB(txn, nss.db(), MODE_X);
        WriteUnitOfWork wunit(txn);
        const auto status = autoDB.getDb()->dropCollection(txn, nss.ns());
        if (status.isOK()) {
            wunit.commit();
        }
        return status;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "StorageInterfaceImpl::dropCollection", nss.ns());
}

Status StorageInterfaceImpl::isAdminDbValid(OperationContext* txn) {
    log() << "StorageInterfaceImpl::isAdminDbValid called.";
    // TODO: plumb through operation context from caller, for now run on ioThread with runner.
    TaskRunner runner(_bulkLoaderThreads.get());
    auto status = runner.runSynchronousTask(
        [](OperationContext* txn) -> Status {
            ScopedTransaction transaction(txn, MODE_IX);
            AutoGetDb autoDB(txn, "admin", MODE_X);
            return checkAdminDatabase(txn, autoDB.getDb());
        },
        TaskRunner::NextAction::kDisposeOperationContext);
    return status;
}

}  // namespace repl
}  // namespace mongo
