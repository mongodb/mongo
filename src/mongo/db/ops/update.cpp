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
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/log.h"

namespace mongo {

    UpdateResult update(OperationContext* txn,
                        Database* db,
                        const UpdateRequest& request,
                        OpDebug* opDebug) {
        invariant(db);

        // Explain should never use this helper.
        invariant(!request.isExplain());

        const NamespaceString& nsString = request.getNamespaceString();
        Collection* collection = db->getCollection(nsString.ns());

        // The update stage does not create its own collection.  As such, if the update is
        // an upsert, create the collection that the update stage inserts into beforehand.
        if (!collection && request.isUpsert()) {
            // We have to have an exclusive lock on the db to be allowed to create the collection.
            // Callers should either get an X or create the collection.
            const Locker* locker = txn->lockState();
            invariant(locker->isW() ||
                      locker->isLockHeldForMode(ResourceId(RESOURCE_DATABASE, nsString.db()),
                                                MODE_X));

            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock lk(txn->lockState(), nsString.db(), MODE_X);

            bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
                !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db());

            if (userInitiatedWritesAndNotPrimary) {
                uassertStatusOK(Status(ErrorCodes::NotMaster, str::stream()
                    << "Not primary while creating collection " << nsString.ns()
                    << " during upsert"));
            }

            WriteUnitOfWork wuow(txn);
            collection = db->createCollection(txn, nsString.ns(), CollectionOptions());
            invariant(collection);

            wuow.commit();
        }

        // Parse the update, get an executor for it, run the executor, get stats out.
        ParsedUpdate parsedUpdate(txn, &request);
        uassertStatusOK(parsedUpdate.parseRequest());

        PlanExecutor* rawExec;
        uassertStatusOK(getExecutorUpdate(txn, collection, &parsedUpdate, opDebug, &rawExec));
        boost::scoped_ptr<PlanExecutor> exec(rawExec);

        uassertStatusOK(exec->executePlan());
        return UpdateStage::makeUpdateResult(exec.get(), opDebug);
    }

    BSONObj applyUpdateOperators(const BSONObj& from, const BSONObj& operators) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        Status status = driver.parse(operators);
        if (!status.isOK()) {
            uasserted(16838, status.reason());
        }

        mutablebson::Document doc(from, mutablebson::Document::kInPlaceDisabled);
        status = driver.update(StringData(), &doc);
        if (!status.isOK()) {
            uasserted(16839, status.reason());
        }

        return doc.getObject();
    }

}  // namespace mongo
