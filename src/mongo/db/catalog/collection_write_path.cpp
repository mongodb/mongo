/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_write_path.h"

#include "mongo/db/catalog/capped_collection_maintenance.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace collection_internal {
namespace {

// This failpoint throws a WriteConflictException after a successful call to
// insertDocumentForBulkLoader
MONGO_FAIL_POINT_DEFINE(failAfterBulkLoadDocInsert);

//  This fail point injects insertion failures for all collections unless a collection name is
//  provided in the optional data object during configuration:
//  data: {
//      collectionNS: <fully-qualified collection namespace>,
//  }
MONGO_FAIL_POINT_DEFINE(failCollectionInserts);

}  // namespace

Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   const BSONObj& doc,
                                   const OnRecordInsertedFn& onRecordInserted) {
    const auto& nss = collection->ns();
    const auto& uuid = collection->uuid();

    auto status = collection_internal::checkFailCollectionInsertsFailPoint(nss, doc);
    if (!status.isOK()) {
        return status;
    }

    status = collection->checkValidationAndParseResult(opCtx, doc);
    if (!status.isOK()) {
        return status;
    }

    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    RecordId recordId;
    if (collection->isClustered()) {
        invariant(collection->getRecordStore()->keyFormat() == KeyFormat::String);
        recordId = uassertStatusOK(record_id_helpers::keyForDoc(
            doc, collection->getClusteredInfo()->getIndexSpec(), collection->getDefaultCollator()));
    }

    // Using timestamp 0 for these inserts, which are non-oplog so we don't have an appropriate
    // timestamp to use.
    StatusWith<RecordId> loc = collection->getRecordStore()->insertRecord(
        opCtx, recordId, doc.objdata(), doc.objsize(), Timestamp());

    if (!loc.isOK())
        return loc.getStatus();

    status = onRecordInserted(loc.getValue());

    if (MONGO_unlikely(failAfterBulkLoadDocInsert.shouldFail())) {
        LOGV2(20290,
              "Failpoint failAfterBulkLoadDocInsert enabled. Throwing "
              "WriteConflictException",
              logAttrs(nss));
        throwWriteConflictException();
    }

    std::vector<InsertStatement> inserts;
    OplogSlot slot;
    // Fetch a new optime now, if necessary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isOplogDisabledFor(opCtx, nss)) {
        auto slots = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1);
        slot = slots.back();
    }
    inserts.emplace_back(kUninitializedStmtId, doc, slot);

    opCtx->getServiceContext()->getOpObserver()->onInserts(
        opCtx, nss, uuid, inserts.begin(), inserts.end(), false);

    collection_internal::cappedDeleteUntilBelowConfiguredMaximum(opCtx, collection, loc.getValue());

    // Capture the recordStore here instead of the CollectionPtr object itself, because the record
    // store's lifetime is controlled by the collection IX lock held on the write paths, whereas the
    // CollectionPtr is just a front to the collection and its lifetime is shorter
    opCtx->recoveryUnit()->onCommit(
        [recordStore = collection->getRecordStore()](boost::optional<Timestamp>) {
            recordStore->notifyCappedWaitersIfNeeded();
        });

    return loc.getStatus();
}

Status checkFailCollectionInsertsFailPoint(const NamespaceString& ns, const BSONObj& firstDoc) {
    Status s = Status::OK();
    failCollectionInserts.executeIf(
        [&](const BSONObj& data) {
            const std::string msg = str::stream()
                << "Failpoint (failCollectionInserts) has been enabled (" << data
                << "), so rejecting insert (first doc): " << firstDoc;
            LOGV2(20287,
                  "Failpoint (failCollectionInserts) has been enabled, so rejecting insert",
                  "data"_attr = data,
                  "document"_attr = firstDoc);
            s = {ErrorCodes::FailPointEnabled, msg};
        },
        [&](const BSONObj& data) {
            // If the failpoint specifies no collection or matches the existing one, fail.
            const auto collElem = data["collectionNS"];
            return !collElem || ns.ns() == collElem.str();
        });
    return s;
}

}  // namespace collection_internal
}  // namespace mongo
