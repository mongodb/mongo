// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/dbcheck/deferred_writer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <compare>
#include <functional>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {

namespace {
auto kLogInterval = std::chrono::minutes(1);
}

void DeferredWriter::_logFailure(const Status& status) {
    if (TimePoint::clock::now() - _lastLogged > kLogInterval) {
        LOGV2(20516, "Unable to write to collection", logAttrs(_nss), "error"_attr = status);
        _lastLogged = std::chrono::system_clock::now();
    }
}

void DeferredWriter::_logDroppedEntry() {
    _droppedEntries += 1;
    if (TimePoint::clock::now() - _lastLoggedDrop > kLogInterval) {
        LOGV2(20517,
              "Deferred write buffer for {namespace} is full. {droppedEntries} entries have been "
              "dropped.",
              logAttrs(_nss),
              "droppedEntries"_attr = _droppedEntries);
        _lastLoggedDrop = std::chrono::system_clock::now();
        _droppedEntries = 0;
    }
}

Status DeferredWriter::_makeCollection(OperationContext* opCtx) {
    BSONObjBuilder builder;
    builder.append("create", _nss.coll());
    builder.appendElements(_collectionOptions.toBSON());
    try {
        return createCollection(opCtx, _nss.dbName(), builder.obj().getOwned());
    } catch (const DBException& exception) {
        return exception.toStatus();
    }
}

StatusWith<CollectionAcquisition> DeferredWriter::_getCollection(OperationContext* opCtx) {
    while (true) {
        {
            auto collection =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      _nss,
                                      PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            if (collection.exists()) {
                return std::move(collection);
            }
        }

        // Release the lockS before trying to rebuild the collection.
        Status status = _makeCollection(opCtx);
        if (!status.isOK()) {
            return status;
        }
    }
}

Status DeferredWriter::_worker(BSONObj doc) noexcept try {
    auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();
    auto result = _getCollection(opCtx);

    if (!result.isOK()) {
        return result.getStatus();
    }

    const auto collection = std::move(result.getValue());

    Status status = writeConflictRetry(opCtx, "deferred insert", _nss, [&] {
        WriteUnitOfWork wuow(opCtx);
        Status status = Helpers::insert(opCtx, collection.getCollectionPtr(), doc);
        if (!status.isOK()) {
            return status;
        }

        wuow.commit();
        return Status::OK();
    });

    std::lock_guard<std::mutex> lock(_mutex);

    _numBytes -= doc.objsize();
    return status;
} catch (const DBException& e) {
    return e.toStatus();
}

DeferredWriter::DeferredWriter(NamespaceString nss,
                               CollectionOptions opts,
                               int64_t maxSize,
                               bool retryOnReplStateChangeInterruption)
    : _collectionOptions(opts),
      _maxNumBytes(maxSize),
      _nss(nss),
      _numBytes(0),
      _droppedEntries(0),
      _lastLogged(TimePoint::clock::now() - kLogInterval),
      _retryOnReplStateChangeInterruption(retryOnReplStateChangeInterruption) {}

DeferredWriter::~DeferredWriter() {}

void DeferredWriter::startup(std::string workerName) {
    // We should only start up once.
    invariant(!_pool);
    _pool = ThreadPool::make({
        .poolName = "deferred writer pool",
        .threadNamePrefix = workerName,
        .minThreads = 0,
        .maxThreads = 1,
        .onCreateThread =
            [](const std::string& name) {
                Client::initThread(name, getGlobalServiceContext()->getService());
            },
    });
    _pool->startup();
}

void DeferredWriter::shutdown(void) {
    // If we never allocated the pool, no cleanup is necessary.
    if (!_pool) {
        return;
    }

    _pool->waitForIdle();
    _pool->shutdown();
    _pool->join();
}

bool DeferredWriter::insertDocument(BSONObj obj) {
    // We can't insert documents if we haven't been started up.
    invariant(_pool);

    std::lock_guard<std::mutex> lock(_mutex);

    // Check if we're allowed to insert this object.
    if (_numBytes + obj.objsize() >= _maxNumBytes) {
        // If not, drop it.  We always drop new entries rather than old ones; that way the
        // caller knows at the time of the call that the entry was dropped.
        _logDroppedEntry();
        return false;
    }

    // Add the object to the buffer.
    _numBytes += obj.objsize();
    _pool->schedule([this, obj](auto status) {
        fassert(40588, status);
        bool retryable;
        int numRetries = 5;
        do {
            retryable = false;
            auto workerStatus = _worker(obj.getOwned());
            if (workerStatus.isOK()) {
                break;
            }

            _logFailure(workerStatus);
            retryable = _retryOnReplStateChangeInterruption &&
                workerStatus.code() == ErrorCodes::InterruptedDueToReplStateChange;
        } while (retryable && numRetries-- > 0);
    });
    return true;
}

int64_t DeferredWriter::getDroppedEntries() {
    return _droppedEntries;
}


}  // namespace mongo
