// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/write_ops/update.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/none.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

UpdateResult doUpdate(OperationContext* opCtx,
                      CollectionAcquisition& coll,
                      const UpdateRequest& request) {
    // Explain should never use this helper.
    tassert(11052009, "Unexpected explain on UpdateRequest", !request.explain());

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
            tassert(11052010,
                    fmt::format("Expected collection {} to be created", nsString.coll()),
                    newCollectionPtr);
            wuow.commit();
        }
    });

    // If this is an upsert, at this point the collection must exist.
    tassert(11052011,
            fmt::format("Expected collection {} to exist for an upsert operation", nsString.coll()),
            coll.exists() || !request.isUpsert());

    auto [collatorToUse, expCtxCollationMatchesDefault] =
        resolveCollator(opCtx, request.getCollation(), coll.getCollectionPtr());

    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, request)
                      .collator(std::move(collatorToUse))
                      .collationMatchesDefault(expCtxCollationMatchesDefault)
                      .build();

    // Parse the update, get an executor for it, run the executor, get stats out.
    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx,
        &request,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx, &request.getNsString())));

    auto canonicalUpdate = uassertStatusOK(
        CanonicalUpdate::make(expCtx, std::move(parsedUpdate), coll.getCollectionPtr()));

    OpDebug* const nullOpDebug = nullptr;
    auto exec = uassertStatusOK(
        getExecutorUpdate(nullOpDebug, coll, *canonicalUpdate, boost::none /* verbosity */));

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
