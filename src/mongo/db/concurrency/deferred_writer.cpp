/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/db/concurrency/deferred_writer.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
auto kLogInterval = stdx::chrono::minutes(1);
}

void DeferredWriter::_logFailure(const Status& status) {
    if (TimePoint::clock::now() - _lastLogged > kLogInterval) {
        log() << "Unable to write to collection " << _nss.toString() << ": " << status.toString();
        _lastLogged = stdx::chrono::system_clock::now();
    }
}

void DeferredWriter::_logDroppedEntry() {
    _droppedEntries += 1;
    if (TimePoint::clock::now() - _lastLoggedDrop > kLogInterval) {
        log() << "Deferred write buffer for " << _nss.toString() << " is full. " << _droppedEntries
              << " entries have been dropped.";
        _lastLoggedDrop = stdx::chrono::system_clock::now();
        _droppedEntries = 0;
    }
}

Status DeferredWriter::_makeCollection(OperationContext* opCtx) {
    BSONObjBuilder builder;
    builder.append("create", _nss.coll());
    builder.appendElements(_collectionOptions.toBSON());
    try {
        return createCollection(opCtx, _nss.db().toString(), builder.obj().getOwned());
    } catch (const DBException& exception) {
        return exception.toStatus();
    }
}

StatusWith<std::unique_ptr<AutoGetCollection>> DeferredWriter::_getCollection(
    OperationContext* opCtx) {
    std::unique_ptr<AutoGetCollection> agc;
    agc = stdx::make_unique<AutoGetCollection>(opCtx, _nss, MODE_IX);

    while (!agc->getCollection()) {
        // Release the previous AGC's lock before trying to rebuild the collection.
        agc.reset();
        Status status = _makeCollection(opCtx);

        if (!status.isOK()) {
            return status;
        }

        agc = stdx::make_unique<AutoGetCollection>(opCtx, _nss, MODE_IX);
    }

    return std::move(agc);
}

void DeferredWriter::_worker(InsertStatement stmt) {
    auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();
    auto result = _getCollection(opCtx);

    if (!result.isOK()) {
        _logFailure(result.getStatus());
        return;
    }

    auto agc = std::move(result.getValue());

    Collection& collection = *agc->getCollection();

    Status status = writeConflictRetry(opCtx, "deferred insert", _nss.ns(), [&] {
        WriteUnitOfWork wuow(opCtx);
        Status status = collection.insertDocument(opCtx, stmt, nullptr, false);
        if (!status.isOK()) {
            return status;
        }

        wuow.commit();
        return Status::OK();
    });

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _numBytes -= stmt.doc.objsize();

    // If a write to a deferred collection fails, periodically tell the log.
    if (!status.isOK()) {
        _logFailure(status);
    }
}

DeferredWriter::DeferredWriter(NamespaceString nss, CollectionOptions opts, int64_t maxSize)
    : _collectionOptions(opts),
      _maxNumBytes(maxSize),
      _nss(nss),
      _numBytes(0),
      _droppedEntries(0),
      _lastLogged(TimePoint::clock::now() - kLogInterval) {}

DeferredWriter::~DeferredWriter() {}

void DeferredWriter::startup(std::string workerName) {
    // We should only start up once.
    invariant(!_pool);
    ThreadPool::Options options;
    options.poolName = "deferred writer pool";
    options.threadNamePrefix = workerName;
    options.minThreads = 0;
    options.maxThreads = 1;
    options.onCreateThread = [](const std::string& name) { Client::initThread(name); };
    _pool = stdx::make_unique<ThreadPool>(options);
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

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // Check if we're allowed to insert this object.
    if (_numBytes + obj.objsize() >= _maxNumBytes) {
        // If not, drop it.  We always drop new entries rather than old ones; that way the caller
        // knows at the time of the call that the entry was dropped.
        _logDroppedEntry();
        return false;
    }

    // Add the object to the buffer.
    _numBytes += obj.objsize();
    fassertStatusOK(40588,
                    _pool->schedule([this, obj] { _worker(InsertStatement(obj.getOwned())); }));
    return true;
}

int64_t DeferredWriter::getDroppedEntries() {
    return _droppedEntries;
}


}  // namespace mongo
