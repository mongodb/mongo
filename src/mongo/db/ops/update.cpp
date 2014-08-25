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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/ops/update.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_executor.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace {

        // TODO: Make this a function on NamespaceString, or make it cleaner.
        inline void validateUpdate(const char* ns ,
                                   const BSONObj& updateobj,
                                   const BSONObj& patternOrig) {
            uassert(10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0);
            if (strstr(ns, ".system.")) {
                /* dm: it's very important that system.indexes is never updated as IndexDetails
                   has pointers into it */
                uassert(10156,
                         str::stream() << "cannot update system collection: "
                         << ns << " q: " << patternOrig << " u: " << updateobj,
                         legalClientSystemNS(ns , true));
            }
        }

    } // namespace

    UpdateResult update(Database* db,
                        const UpdateRequest& request,
                        OpDebug* opDebug) {

        UpdateExecutor executor(&request, opDebug);
        return executor.execute(db);
    }

    UpdateResult update(Database* db,
                        const UpdateRequest& request,
                        OpDebug* opDebug,
                        UpdateDriver* driver,
                        CanonicalQuery* cq) {

        LOG(3) << "processing update : " << request;

        std::auto_ptr<CanonicalQuery> cqHolder(cq);
        const NamespaceString& nsString = request.getNamespaceString();
        UpdateLifecycle* lifecycle = request.getLifecycle();

        Collection* collection = db->getCollection(request.getOpCtx(), nsString.ns());

        validateUpdate(nsString.ns().c_str(), request.getUpdates(), request.getQuery());


        // TODO: This seems a bit circuitious.
        opDebug->updateobj = request.getUpdates();

        if (lifecycle) {
            lifecycle->setCollection(collection);
            driver->refreshIndexKeys(lifecycle->getIndexKeys(request.getOpCtx()));
        }

        PlanExecutor* rawExec;
        Status status = Status::OK();
        if (cq) {
            // This is the regular path for when we have a CanonicalQuery.
            status = getExecutorUpdate(request.getOpCtx(), db, cqHolder.release(), &request, driver,
                                       opDebug, &rawExec);
        }
        else {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work
            // to create a CanonicalQuery.
            status = getExecutorUpdate(request.getOpCtx(), db, nsString.ns(), &request, driver,
                                       opDebug, &rawExec);
        }

        uassert(17243,
                "could not get executor" + request.getQuery().toString() + "; " + causedBy(status),
                status.isOK());

        // Create the plan executor and setup all deps.
        scoped_ptr<PlanExecutor> exec(rawExec);

        // Register executor with the collection cursor cache.
        const ScopedExecutorRegistration safety(exec.get());

        // Run the plan (don't need to collect results because UpdateStage always returns
        // NEED_TIME).
        uassertStatusOK(exec->executePlan());

        // Get stats from the root stage.
        invariant(exec->getRootStage()->stageType() == STAGE_UPDATE);
        UpdateStage* updateStage = static_cast<UpdateStage*>(exec->getRootStage());
        const UpdateStats* updateStats =
            static_cast<const UpdateStats*>(updateStage->getSpecificStats());

        // Use stats from the root stage to fill out opDebug.
        opDebug->nMatched = updateStats->nMatched;
        opDebug->nModified = updateStats->nModified;
        opDebug->upsert = updateStats->inserted;
        opDebug->fastmodinsert = updateStats->fastmodinsert;
        opDebug->fastmod = updateStats->fastmod;

        // Historically, 'opDebug' considers 'nMatched' and 'nModified' to be 1 (rather than 0) if
        // there is an upsert that inserts a document. The UpdateStage does not participate in this
        // madness in order to have saner stats reporting for explain. This means that we have to
        // set these values "manually" in the case of an insert.
        if (updateStats->inserted) {
            opDebug->nMatched = 1;
            opDebug->nModified = 1;
        }

        // Get summary information about the plan.
        PlanSummaryStats stats;
        Explain::getSummaryStats(exec.get(), &stats);
        opDebug->nscanned = stats.totalKeysExamined;
        opDebug->nscannedObjects = stats.totalDocsExamined;

        return UpdateResult(updateStats->nMatched > 0 /* Did we update at least one obj? */,
                            !driver->isDocReplacement() /* $mod or obj replacement */,
                            opDebug->nModified /* number of modified docs, no no-ops */,
                            opDebug->nMatched /* # of docs matched/updated, even no-ops */,
                            updateStats->objInserted);
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
