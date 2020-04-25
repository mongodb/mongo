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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/ops/update.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

UpdateResult update(OperationContext* opCtx, Database* db, const UpdateRequest& request) {
    invariant(db);

    // Explain should never use this helper.
    invariant(!request.isExplain());

    const NamespaceString& nsString = request.getNamespaceString();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nsString, MODE_IX));

    Collection* collection;

    // The update stage does not create its own collection.  As such, if the update is
    // an upsert, create the collection that the update stage inserts into beforehand.
    writeConflictRetry(opCtx, "createCollection", nsString.ns(), [&] {
        collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nsString);
        if (collection || !request.isUpsert()) {
            return;
        }

        const bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nsString);

        if (userInitiatedWritesAndNotPrimary) {
            uassertStatusOK(Status(ErrorCodes::PrimarySteppedDown,
                                   str::stream() << "Not primary while creating collection "
                                                 << nsString << " during upsert"));
        }
        WriteUnitOfWork wuow(opCtx);
        collection = db->createCollection(opCtx, nsString, CollectionOptions());
        invariant(collection);
        wuow.commit();
    });

    // Parse the update, get an executor for it, run the executor, get stats out.
    const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
    ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
    uassertStatusOK(parsedUpdate.parseRequest());

    OpDebug* const nullOpDebug = nullptr;
    auto exec = uassertStatusOK(
        getExecutorUpdate(nullOpDebug, collection, &parsedUpdate, boost::none /* verbosity */));

    uassertStatusOK(exec->executePlan());

    const UpdateStats* updateStats = UpdateStage::getUpdateStats(exec.get());

    return UpdateStage::makeUpdateResult(updateStats);
}

BSONObj applyUpdateOperators(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& from,
                             const BSONObj& operators) {
    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);
    UpdateDriver driver(std::move(expCtx));
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    driver.parse(operators, arrayFilters);

    mutablebson::Document doc(from, mutablebson::Document::kInPlaceDisabled);

    const bool validateForStorage = false;
    const FieldRefSet emptyImmutablePaths;
    const bool isInsert = false;

    uassertStatusOK(
        driver.update(StringData(), &doc, validateForStorage, emptyImmutablePaths, isInsert));

    return doc.getObject();
}

}  // namespace mongo
