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

#include "mongo/db/repl/oplog_writer_impl.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/storage/storage_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

const auto changeCollNss = NamespaceString::makeChangeCollectionNSS(boost::none);

Status insertDocsToOplogCollection(OperationContext* opCtx,
                                   std::vector<InsertStatement>::const_iterator begin,
                                   std::vector<InsertStatement>::const_iterator end) {
    WriteUnitOfWork wuow(opCtx);

    // Acquire the collection lock.
    AutoGetOplog autoOplog(opCtx, OplogAccessMode::kWrite);
    auto& oplogColl = autoOplog.getCollection();
    if (!oplogColl) {
        return {ErrorCodes::NamespaceNotFound, "Oplog collection does not exist"};
    }

    auto status = collection_internal::insertDocuments(
        opCtx, oplogColl, begin, end, nullptr /* OpDebug */, false /* fromMigrate */);
    if (!status.isOK()) {
        return status;
    }

    wuow.commit();

    return Status::OK();
}

Status insertDocsToChangeCollection(OperationContext* opCtx,
                                    std::vector<InsertStatement>::const_iterator begin,
                                    std::vector<InsertStatement>::const_iterator end) {
    WriteUnitOfWork wuow(opCtx);

    // Acquire the collection lock.
    auto writer = ChangeStreamChangeCollectionManager::get(opCtx).createChangeCollectionsWriter(
        opCtx, begin, end, nullptr /* opDebug */);

    writer.acquireLocks();

    auto status = writer.write();
    if (!status.isOK()) {
        return status;
    }

    wuow.commit();

    return Status::OK();
}

}  // namespace

OplogWriterImpl::OplogWriterImpl(ReplicationCoordinator* replCoord,
                                 StorageInterface* storageInterface,
                                 ThreadPool* writerPool,
                                 Observer* observer,
                                 const OplogWriter::Options& options)
    : OplogWriter(options),
      _replCoord(replCoord),
      _storageInterface(storageInterface),
      _writerPool(writerPool),
      _observer(observer) {}

StatusWith<OpTime> OplogWriterImpl::writeOplogBatch(OperationContext* opCtx,
                                                    std::vector<BSONObj> ops) {
    invariant(!ops.empty());
    LOGV2_DEBUG(8352100, 2, "Oplog write batch size", "size"_attr = ops.size());

    std::vector<InsertStatement> docs;
    bool writeChangeCollection = change_stream_serverless_helpers::isChangeCollectionsModeActive();

    // Create insert statements from the oplog entries.
    docs.reserve(ops.size());
    for (const auto& op : ops) {
        auto opTime = invariantStatusOK(OpTime::parseFromOplogEntry(op));
        docs.emplace_back(InsertStatement{op, opTime.getTimestamp(), opTime.getTerm()});
    }

    // Write to the change collection in a separate thread and separate storage transaction.
    // This step can be skipped if not running in serverless.
    if (writeChangeCollection) {
        _writerPool->schedule([this, &docs](auto status) {
            invariant(status);
            auto newOpCtx = cc().makeOperationContext();
            _writeOplogBatchImpl(newOpCtx.get(), docs, changeCollNss, insertDocsToChangeCollection);
            _observer->onWriteChangeCollection(docs);
        });
    }

    // Write to the oplog collection in the current thread to avoid thread pool overheads.
    // This step can be skipped during startup recovery.
    if (!getOptions().skipWritesToOplogColl) {
        _writeOplogBatchImpl(
            opCtx, docs, NamespaceString::kRsOplogNamespace, insertDocsToOplogCollection);
        _observer->onWriteOplogCollection(docs);
    }

    // Wait for writes to the serverless change collection to complete.
    if (writeChangeCollection) {
        _writerPool->waitForIdle();
    }

    return docs.back().oplogSlot;
}

void OplogWriterImpl::_writeOplogBatchImpl(OperationContext* opCtx,
                                           const std::vector<InsertStatement>& docs,
                                           const NamespaceString& nss,
                                           writeDocsFn&& writeDocsFn) {
    // Oplog writes are crucial to the stability of the replica set. We give the operations
    // Immediate priority so that it skips waiting for ticket acquisition and flow control.
    ScopedAdmissionPriorityForLock priority(shard_role_details::getLocker(opCtx),
                                            AdmissionContext::Priority::kImmediate);
    UnreplicatedWritesBlock uwb(opCtx);

    fassert(8352101,
            storage_helpers::insertBatchAndHandleRetry(
                opCtx, nss, docs, [&](auto* opCtx, auto begin, auto end) {
                    return writeDocsFn(opCtx, begin, end);
                }));
}

}  // namespace repl
}  // namespace mongo
