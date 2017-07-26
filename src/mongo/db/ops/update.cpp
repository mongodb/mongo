//@file update.cpp

/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/ops/update.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

UpdateResult update(OperationContext* opCtx, Database* db, const UpdateRequest& request) {
    invariant(db);

    // Explain should never use this helper.
    invariant(!request.isExplain());

    const NamespaceString& nsString = request.getNamespaceString();
    Collection* collection = db->getCollection(opCtx, nsString);

    // The update stage does not create its own collection.  As such, if the update is
    // an upsert, create the collection that the update stage inserts into beforehand.
    if (!collection && request.isUpsert()) {
        // We have to have an exclusive lock on the db to be allowed to create the collection.
        // Callers should either get an X or create the collection.
        const Locker* locker = opCtx->lockState();
        invariant(locker->isW() ||
                  locker->isLockHeldForMode(ResourceId(RESOURCE_DATABASE, nsString.db()), MODE_X));

        writeConflictRetry(opCtx, "createCollection", nsString.ns(), [&] {
            Lock::DBLock lk(opCtx, nsString.db(), MODE_X);

            const bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
                !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nsString);

            if (userInitiatedWritesAndNotPrimary) {
                uassertStatusOK(Status(ErrorCodes::PrimarySteppedDown,
                                       str::stream() << "Not primary while creating collection "
                                                     << nsString.ns()
                                                     << " during upsert"));
            }
            WriteUnitOfWork wuow(opCtx);
            collection = db->createCollection(opCtx, nsString.ns(), CollectionOptions());
            invariant(collection);
            wuow.commit();
        });
    }

    // Parse the update, get an executor for it, run the executor, get stats out.
    ParsedUpdate parsedUpdate(opCtx, &request);
    uassertStatusOK(parsedUpdate.parseRequest());

    OpDebug* const nullOpDebug = nullptr;
    auto exec = uassertStatusOK(getExecutorUpdate(opCtx, nullOpDebug, collection, &parsedUpdate));

    uassertStatusOK(exec->executePlan());

    const UpdateStats* updateStats = UpdateStage::getUpdateStats(exec.get());

    return UpdateStage::makeUpdateResult(updateStats);
}

BSONObj applyUpdateOperators(const BSONObj& from, const BSONObj& operators) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    Status status = driver.parse(operators, arrayFilters);
    if (!status.isOK()) {
        uasserted(16838, status.reason());
    }

    mutablebson::Document doc(from, mutablebson::Document::kInPlaceDisabled);

    // The original document can be empty because it is only needed for validation of immutable
    // paths.
    const BSONObj emptyOriginal;
    const bool validateForStorage = false;
    const FieldRefSet emptyImmutablePaths;
    status =
        driver.update(StringData(), emptyOriginal, &doc, validateForStorage, emptyImmutablePaths);
    if (!status.isOK()) {
        uasserted(16839, status.reason());
    }

    return doc.getObject();
}

}  // namespace mongo
