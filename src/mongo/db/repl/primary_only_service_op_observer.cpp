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
                                                 NamespaceString const& nss,
                                                 BSONObj const& doc) {
    // Extract the _id field from the document. If it does not have an _id, use the
    // document itself as the _id.
    documentIdDecoration(opCtx) = doc["_id"] ? doc["_id"].wrap() : doc;
}

void PrimaryOnlyServiceOpObserver::onDelete(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            OptionalCollectionUUID uuid,
                                            StmtId stmtId,
                                            bool fromMigrate,
                                            const boost::optional<BSONObj>& deletedDoc) {
    auto& documentId = documentIdDecoration(opCtx);
    invariant(!documentId.isEmpty());

    auto service = _registry->lookupServiceByNamespace(nss);
    if (!service) {
        return;
    }
    service->releaseInstance(documentId);
}


repl::OpTime PrimaryOnlyServiceOpObserver::onDropCollection(OperationContext* opCtx,
                                                            const NamespaceString& collectionName,
                                                            OptionalCollectionUUID uuid,
                                                            std::uint64_t numRecords,
                                                            const CollectionDropType dropType) {
    auto service = _registry->lookupServiceByNamespace(collectionName);
    if (service) {
        service->releaseAllInstances();
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
