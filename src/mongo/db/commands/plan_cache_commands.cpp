/**
*    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <string>
#include <sstream>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/util/log.h"

namespace {

    using std::string;
    using namespace mongo;

    /**
     * Utility function to extract error code and message from status
     * and append to BSON results.
     */
    void addStatus(const Status& status, BSONObjBuilder& builder) {
        builder.append("ok", status.isOK() ? 1.0 : 0.0);
        if (!status.isOK()) {
            builder.append("code", status.code());
        }
        if (!status.reason().empty()) {
            builder.append("errmsg", status.reason());
        }
    }

    /**
     * Retrieves a collection's plan cache from the database.
     */
    Status getPlanCache(OperationContext* txn, Database* db, const string& ns, PlanCache** planCacheOut) {
        invariant(db);

        Collection* collection = db->getCollection(txn, ns);
        if (NULL == collection) {
            return Status(ErrorCodes::BadValue, "no such collection");
        }

        CollectionInfoCache* infoCache = collection->infoCache();
        invariant(infoCache);

        PlanCache* planCache = infoCache->getPlanCache();
        invariant(planCache);

        *planCacheOut = planCache;
        return Status::OK();
    }

    //
    // Command instances.
    // Registers commands with the command system and make commands
    // available to the client.
    //

    MONGO_INITIALIZER_WITH_PREREQUISITES(SetupPlanCacheCommands, MONGO_NO_PREREQUISITES)(
            InitializerContext* context) {

        // PlanCacheCommand constructors refer to static ActionType instances.
        // Registering commands in a mongo static initializer ensures that
        // the ActionType construction will be completed first.
        new PlanCacheListQueryShapes();
        new PlanCacheClear();
        new PlanCacheListPlans();

        return Status::OK();
    }

} // namespace

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kCommands);

    using std::string;
    using std::stringstream;
    using std::vector;
    using boost::scoped_ptr;

    PlanCacheCommand::PlanCacheCommand(const string& name, const string& helpText,
                                       ActionType actionType)
        : Command(name),
          helpText(helpText),
          actionType(actionType) { }

    bool PlanCacheCommand::run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int options,
                               string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        string ns = parseNs(dbname, cmdObj);

        Status status = runPlanCacheCommand(txn, ns, cmdObj, &result);

        if (!status.isOK()) {
            addStatus(status, result);
            return false;
        }

        return true;
    }

    bool PlanCacheCommand::isWriteCommandForConfigServer() const { return false; }

    bool PlanCacheCommand::slaveOk() const {
        return false;
    }

    void PlanCacheCommand::help(stringstream& ss) const {
        ss << helpText;
    }

    Status PlanCacheCommand::checkAuthForCommand(ClientBasic* client,
                                                 const std::string& dbname,
                                                 const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = client->getAuthorizationSession();
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, actionType)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // static
    Status PlanCacheCommand::canonicalize(OperationContext* txn,
                                          const string& ns,
                                          const BSONObj& cmdObj,
                                          CanonicalQuery** canonicalQueryOut) {
        // query - required
        BSONElement queryElt = cmdObj.getField("query");
        if (queryElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field query missing");
        }
        if (!queryElt.isABSONObj()) {
            return Status(ErrorCodes::BadValue, "required field query must be an object");
        }
        if (queryElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field query missing");
        }
        BSONObj queryObj = queryElt.Obj();

        // sort - optional
        BSONElement sortElt = cmdObj.getField("sort");
        BSONObj sortObj;
        if (!sortElt.eoo()) {
            if (!sortElt.isABSONObj()) {
                return Status(ErrorCodes::BadValue, "optional field sort must be an object");
            }
            sortObj = sortElt.Obj();
        }

        // projection - optional
        BSONElement projElt = cmdObj.getField("projection");
        BSONObj projObj;
        if (!projElt.eoo()) {
            if (!projElt.isABSONObj()) {
                return Status(ErrorCodes::BadValue, "optional field projection must be an object");
            }
            projObj = projElt.Obj();
        }

        // Create canonical query
        CanonicalQuery* cqRaw;

        const NamespaceString nss(ns);
        const WhereCallbackReal whereCallback(txn, nss.db());

        Status result = CanonicalQuery::canonicalize(
                            ns, queryObj, sortObj, projObj, &cqRaw, whereCallback);
        if (!result.isOK()) {
            return result;
        }

        *canonicalQueryOut = cqRaw;
        return Status::OK();
    }

    PlanCacheListQueryShapes::PlanCacheListQueryShapes() : PlanCacheCommand("planCacheListQueryShapes",
        "Displays all query shapes in a collection.",
        ActionType::planCacheRead) { }

    Status PlanCacheListQueryShapes::runPlanCacheCommand(OperationContext* txn,
                                                         const string& ns,
                                                         BSONObj& cmdObj,
                                                         BSONObjBuilder* bob) {
        // This is a read lock. The query cache is owned by the collection.
        Client::ReadContext readCtx(txn, ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(txn, ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            // No collection - return results with empty shapes array.
            BSONArrayBuilder arrayBuilder(bob->subarrayStart("shapes"));
            arrayBuilder.doneFast();
            return Status::OK();
        }
        return list(*planCache, bob);
    }

    // static
    Status PlanCacheListQueryShapes::list(const PlanCache& planCache, BSONObjBuilder* bob) {
        invariant(bob);

        // Fetch all cached solutions from plan cache.
        vector<PlanCacheEntry*> solutions = planCache.getAllEntries();

        BSONArrayBuilder arrayBuilder(bob->subarrayStart("shapes"));
        for (vector<PlanCacheEntry*>::const_iterator i = solutions.begin(); i != solutions.end(); i++) {
            PlanCacheEntry* entry = *i;
            invariant(entry);

            BSONObjBuilder shapeBuilder(arrayBuilder.subobjStart());
            shapeBuilder.append("query", entry->query);
            shapeBuilder.append("sort", entry->sort);
            shapeBuilder.append("projection", entry->projection);
            shapeBuilder.doneFast();

            // Release resources for cached solution after extracting query shape.
            delete entry;
        }
        arrayBuilder.doneFast();

        return Status::OK();
    }

    PlanCacheClear::PlanCacheClear() : PlanCacheCommand("planCacheClear",
        "Drops one or all cached queries in a collection.",
        ActionType::planCacheWrite) { }

    Status PlanCacheClear::runPlanCacheCommand(OperationContext* txn,
                                               const std::string& ns,
                                               BSONObj& cmdObj,
                                               BSONObjBuilder* bob) {
        // This is a read lock. The query cache is owned by the collection.
        Client::ReadContext readCtx(txn, ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(txn, ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            // No collection - nothing to do. Return OK status.
            return Status::OK();
        }
        return clear(txn, planCache, ns, cmdObj);
    }

    // static
    Status PlanCacheClear::clear(OperationContext* txn,
                                 PlanCache* planCache,
                                 const string& ns,
                                 const BSONObj& cmdObj) {
        invariant(planCache);

        // According to the specification, the planCacheClear command runs in two modes:
        // - clear all query shapes; or
        // - clear plans for single query shape when a query shape is described in the
        //   command arguments.
        if (cmdObj.hasField("query")) {
            CanonicalQuery* cqRaw;
            Status status = PlanCacheCommand::canonicalize(txn, ns, cmdObj, &cqRaw);
            if (!status.isOK()) {
                return status;
            }

            scoped_ptr<CanonicalQuery> cq(cqRaw);

            if (!planCache->contains(*cq)) {
                // Log if asked to clear non-existent query shape.
                LOG(1) << ns << ": query shape doesn't exist in PlanCache - "
                       << cq->getQueryObj().toString()
                       << "(sort: " << cq->getParsed().getSort()
                       << "; projection: " << cq->getParsed().getProj() << ")";
                return Status::OK();
            }

            Status result = planCache->remove(*cq);
            if (!result.isOK()) {
                return result;
            }

            LOG(1) << ns << ": removed plan cache entry - " << cq->getQueryObj().toString()
                   << "(sort: " << cq->getParsed().getSort()
                   << "; projection: " << cq->getParsed().getProj() << ")";

            return Status::OK();
        }

        // If query is not provided, make sure sort and projection are not in arguments.
        // We do not want to clear the entire cache inadvertently when the user
        // forgets to provide a value for "query".
        if (cmdObj.hasField("sort") || cmdObj.hasField("projection")) {
            return Status(ErrorCodes::BadValue, "sort or projection provided without query");
        }

        planCache->clear();

        LOG(1) << ns << ": cleared plan cache";

        return Status::OK();
    }

    PlanCacheListPlans::PlanCacheListPlans() : PlanCacheCommand("planCacheListPlans",
        "Displays the cached plans for a query shape.",
        ActionType::planCacheRead) { }

    Status PlanCacheListPlans::runPlanCacheCommand(OperationContext* txn,
                                                   const std::string& ns,
                                                   BSONObj& cmdObj,
                                                   BSONObjBuilder* bob) {
        Client::ReadContext readCtx(txn, ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(txn, ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            // No collection - return empty plans array.
            BSONArrayBuilder plansBuilder(bob->subarrayStart("plans"));
            plansBuilder.doneFast();
            return Status::OK();
        }
        return list(txn, *planCache, ns, cmdObj, bob);
    }

    // static
    Status PlanCacheListPlans::list(OperationContext* txn,
                                    const PlanCache& planCache,
                                    const std::string& ns,
                                    const BSONObj& cmdObj,
                                    BSONObjBuilder* bob) {
        CanonicalQuery* cqRaw;
        Status status = canonicalize(txn, ns, cmdObj, &cqRaw);
        if (!status.isOK()) {
            return status;
        }

        scoped_ptr<CanonicalQuery> cq(cqRaw);

        if (!planCache.contains(*cq)) {
            // Return empty plans in results if query shape does not
            // exist in plan cache.
            BSONArrayBuilder plansBuilder(bob->subarrayStart("plans"));
            plansBuilder.doneFast();
            return Status::OK();
        }

        PlanCacheEntry* entryRaw;
        Status result = planCache.getEntry(*cq, &entryRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<PlanCacheEntry> entry(entryRaw);

        BSONArrayBuilder plansBuilder(bob->subarrayStart("plans"));
        size_t numPlans = entry->plannerData.size();
        invariant(numPlans == entry->decision->stats.size());
        invariant(numPlans == entry->decision->scores.size());
        for (size_t i = 0; i < numPlans; ++i) {
            BSONObjBuilder planBob(plansBuilder.subobjStart());

            // Create plan details field.
            // Currently, simple string representationg of
            // SolutionCacheData. Need to revisit format when we
            // need to parse user-provided plan details for planCacheAddPlan.
            SolutionCacheData* scd = entry->plannerData[i];
            BSONObjBuilder detailsBob(planBob.subobjStart("details"));
            detailsBob.append("solution", scd->toString());
            detailsBob.doneFast();

            // reason is comprised of score and initial stats provided by
            // multi plan runner.
            BSONObjBuilder reasonBob(planBob.subobjStart("reason"));
            reasonBob.append("score", entry->decision->scores[i]);
            BSONObjBuilder statsBob(reasonBob.subobjStart("stats"));
            PlanStageStats* stats = entry->decision->stats.vector()[i];
            if (stats) {
                statsToBSON(*stats, &statsBob);
            }
            statsBob.doneFast();
            reasonBob.doneFast();

            // BSON object for 'feedback' field is created from query executions
            // and shows number of executions since this cached solution was
            // created as well as score data (average and standard deviation).
            BSONObjBuilder feedbackBob(planBob.subobjStart("feedback"));
            if (i == 0U) {
                feedbackBob.append("nfeedback", int(entry->feedback.size()));
                feedbackBob.append("averageScore", entry->averageScore.get_value_or(0));
                feedbackBob.append("stdDevScore",entry->stddevScore.get_value_or(0));
                BSONArrayBuilder scoresBob(feedbackBob.subarrayStart("scores"));
                for (size_t i = 0; i < entry->feedback.size(); ++i) {
                    BSONObjBuilder scoreBob(scoresBob.subobjStart());
                    scoreBob.append("score", entry->feedback[i]->score);
                }
                scoresBob.doneFast();
            }
            feedbackBob.doneFast();

            planBob.append("filterSet", scd->indexFilterApplied);
        }
        plansBuilder.doneFast();

        return Status::OK();
    }

} // namespace mongo
