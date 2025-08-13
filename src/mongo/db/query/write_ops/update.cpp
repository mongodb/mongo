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


#include "mongo/db/query/write_ops/update.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

UpdateResult update(OperationContext* opCtx,
                    CollectionAcquisition& coll,
                    const UpdateRequest& request) {
    // Explain should never use this helper.
    invariant(!request.explain());

    const NamespaceString& nsString = request.getNamespaceString();
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nsString, MODE_IX));

    // The update stage does not create its own collection.  As such, if the update is
    // an upsert, create the collection that the update stage inserts into beforehand.
    writeConflictRetry(opCtx, "createCollection", nsString, [&] {
        if (!coll.exists() && request.isUpsert()) {
            const bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
                !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nsString);

            if (userInitiatedWritesAndNotPrimary) {
                uassertStatusOK(Status(ErrorCodes::PrimarySteppedDown,
                                       str::stream()
                                           << "Not primary while creating collection "
                                           << nsString.toStringForErrorMsg() << " during upsert"));
            }

            ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &coll);
            WriteUnitOfWork wuow(opCtx);
            auto db = DatabaseHolder::get(opCtx)->openDb(opCtx, coll.nss().dbName());
            auto newCollectionPtr = db->createCollection(opCtx, nsString, CollectionOptions());
            invariant(newCollectionPtr);
            wuow.commit();
        }
    });

    // If this is an upsert, at this point the collection must exist.
    invariant(coll.exists() || !request.isUpsert());

    // Parse the update, get an executor for it, run the executor, get stats out.
    ParsedUpdate parsedUpdate(opCtx, &request, coll.getCollectionPtr());
    uassertStatusOK(parsedUpdate.parseRequest());

    OpDebug* const nullOpDebug = nullptr;
    auto exec = uassertStatusOK(
        getExecutorUpdate(nullOpDebug, coll, &parsedUpdate, boost::none /* verbosity */));

    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    BSONObj image;
    if (request.shouldReturnAnyDocs()) {
        // At the moment, an `UpdateResult` only supports returning one pre/post image
        // document. This function won't disallow multi-updates + returning pre/post images, but
        // will only keep the first one. Additionally, a single call to `getNext` is sufficient for
        // capturing the image.
        state = exec->getNext(&image, nullptr);
    }

    while (state == PlanExecutor::ADVANCED) {
        state = exec->getNext(nullptr, nullptr);
    }

    UpdateResult result = exec->getUpdateResult();
    if (!image.isEmpty()) {
        result.requestedDocImage = image.getOwned();
    }

    return result;
}

}  // namespace mongo
