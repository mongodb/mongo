/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/primary_only_service_op_observer.h"

#include "mongo/db/repl/primary_only_service.h"

namespace mongo {
namespace repl {

namespace {

const auto documentIdDecoration = OperationContext::declareDecoration<BSONObj>();

}  // namespace

PrimaryOnlyServiceOpObserver::PrimaryOnlyServiceOpObserver(ServiceContext* serviceContext) {
    _registry = PrimaryOnlyServiceRegistry::get(serviceContext);
}

PrimaryOnlyServiceOpObserver::~PrimaryOnlyServiceOpObserver() = default;


void PrimaryOnlyServiceOpObserver::aboutToDelete(OperationContext* opCtx,
                                                 const CollectionPtr& coll,
                                                 BSONObj const& doc) {
    // Extract the _id field from the document. If it does not have an _id, use the
    // document itself as the _id.
    documentIdDecoration(opCtx) = doc["_id"] ? doc["_id"].wrap() : doc;
}

void PrimaryOnlyServiceOpObserver::onDelete(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            StmtId stmtId,
                                            const OplogDeleteEntryArgs& args,
                                            OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    auto& documentId = documentIdDecoration(opCtx);
    invariant(!documentId.isEmpty());

    auto service = _registry->lookupServiceByNamespace(nss);
    if (!service) {
        return;
    }
    opCtx->recoveryUnit()->onCommit(
        [service, documentId, nss](OperationContext*, boost::optional<Timestamp>) {
            // Release the instance without interrupting it since for some primary-only services
            // there is still work to be done after the state document is removed.
            service->releaseInstance(documentId, Status::OK());
        });
}


repl::OpTime PrimaryOnlyServiceOpObserver::onDropCollection(OperationContext* opCtx,
                                                            const NamespaceString& collectionName,
                                                            const UUID& uuid,
                                                            std::uint64_t numRecords,
                                                            const CollectionDropType dropType,
                                                            bool markFromMigrate) {
    auto service = _registry->lookupServiceByNamespace(collectionName);
    if (service) {
        opCtx->recoveryUnit()->onCommit(
            [service, collectionName](OperationContext*, boost::optional<Timestamp>) {
                // Release and interrupt all the instances since the state document collection is
                // not supposed to be dropped.
                service->releaseAllInstances(
                    Status(ErrorCodes::Interrupted,
                           str::stream() << collectionName.toStringForErrorMsg() << " is dropped"));
            });
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
