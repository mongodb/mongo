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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <sstream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/util/log.h"

namespace {

using std::string;
using std::unique_ptr;
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
static Status getPlanCache(OperationContext* txn,
                           Collection* collection,
                           const string& ns,
                           PlanCache** planCacheOut) {
    *planCacheOut = NULL;

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

MONGO_INITIALIZER_WITH_PREREQUISITES(SetupPlanCacheCommands, MONGO_NO_PREREQUISITES)
(InitializerContext* context) {
    // PlanCacheCommand constructors refer to static ActionType instances.
    // Registering commands in a mongo static initializer ensures that
    // the ActionType construction will be completed first.
    new PlanCacheListQueryShapes();
    new PlanCacheClear();
    new PlanCacheListPlans();

    return Status::OK();
}

}  // namespace

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;
using std::unique_ptr;

PlanCacheCommand::PlanCacheCommand(const string& name,
                                   const string& helpText,
                                   ActionType actionType)
    : Command(name), helpText(helpText), actionType(actionType) {}

bool PlanCacheCommand::run(OperationContext* txn,
                           const string& dbname,
                           BSONObj& cmdObj,
                           int options,
                           string& errmsg,
                           BSONObjBuilder& result) {
    string ns = parseNs(dbname, cmdObj);

    Status status = runPlanCacheCommand(txn, ns, cmdObj, &result);

    if (!status.isOK()) {
        addStatus(status, result);
        return false;
    }

    return true;
}


bool PlanCacheCommand::supportsWriteConcern(const BSONObj& cmd) const {
    return false;
}

bool PlanCacheCommand::slaveOk() const {
    return false;
}

bool PlanCacheCommand::slaveOverrideOk() const {
    return true;
}

void PlanCacheCommand::help(stringstream& ss) const {
    ss << helpText;
}

Status PlanCacheCommand::checkAuthForCommand(ClientBasic* client,
                                             const std::string& dbname,
                                             const BSONObj& cmdObj) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

    if (authzSession->isAuthorizedForActionsOnResource(pattern, actionType)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

// static
StatusWith<unique_ptr<CanonicalQuery>> PlanCacheCommand::canonicalize(OperationContext* txn,
                                                                      const string& ns,
                                                                      const BSONObj& cmdObj) {
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

    // collation - optional
    BSONObj collationObj;
    if (auto collationElt = cmdObj["collation"]) {
        if (!collationElt.isABSONObj()) {
            return Status(ErrorCodes::BadValue, "optional field collation must be an object");
        }
        collationObj = collationElt.Obj();
        if (collationObj.isEmpty()) {
            return Status(ErrorCodes::BadValue,
                          "optional field collation cannot be an empty object");
        }
    }

    // Create canonical query
    const NamespaceString nss(ns);
    auto qr = stdx::make_unique<QueryRequest>(std::move(nss));
    qr->setFilter(queryObj);
    qr->setSort(sortObj);
    qr->setProj(projObj);
    qr->setCollation(collationObj);
    const ExtensionsCallbackReal extensionsCallback(txn, &nss);
    auto statusWithCQ = CanonicalQuery::canonicalize(txn, std::move(qr), extensionsCallback);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return std::move(statusWithCQ.getValue());
}

PlanCacheListQueryShapes::PlanCacheListQueryShapes()
    : PlanCacheCommand("planCacheListQueryShapes",
                       "Displays all query shapes in a collection.",
                       ActionType::planCacheRead) {}

Status PlanCacheListQueryShapes::runPlanCacheCommand(OperationContext* txn,
                                                     const string& ns,
                                                     BSONObj& cmdObj,
                                                     BSONObjBuilder* bob) {
    // This is a read lock. The query cache is owned by the collection.
    AutoGetCollectionForRead ctx(txn, ns);

    PlanCache* planCache;
    Status status = getPlanCache(txn, ctx.getCollection(), ns, &planCache);
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
        if (!entry->collation.isEmpty()) {
            shapeBuilder.append("collation", entry->collation);
        }
        shapeBuilder.doneFast();

        // Release resources for cached solution after extracting query shape.
        delete entry;
    }
    arrayBuilder.doneFast();

    return Status::OK();
}

PlanCacheClear::PlanCacheClear()
    : PlanCacheCommand("planCacheClear",
                       "Drops one or all cached queries in a collection.",
                       ActionType::planCacheWrite) {}

Status PlanCacheClear::runPlanCacheCommand(OperationContext* txn,
                                           const std::string& ns,
                                           BSONObj& cmdObj,
                                           BSONObjBuilder* bob) {
    // This is a read lock. The query cache is owned by the collection.
    AutoGetCollectionForRead ctx(txn, ns);

    PlanCache* planCache;
    Status status = getPlanCache(txn, ctx.getCollection(), ns, &planCache);
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
        auto statusWithCQ = PlanCacheCommand::canonicalize(txn, ns, cmdObj);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }

        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        if (!planCache->contains(*cq)) {
            // Log if asked to clear non-existent query shape.
            LOG(1) << ns << ": query shape doesn't exist in PlanCache - "
                   << cq->getQueryObj().toString() << "(sort: " << cq->getQueryRequest().getSort()
                   << "; projection: " << cq->getQueryRequest().getProj()
                   << "; collation: " << cq->getQueryRequest().getCollation() << ")";
            return Status::OK();
        }

        Status result = planCache->remove(*cq);
        if (!result.isOK()) {
            return result;
        }

        LOG(1) << ns << ": removed plan cache entry - " << cq->getQueryObj().toString()
               << "(sort: " << cq->getQueryRequest().getSort()
               << "; projection: " << cq->getQueryRequest().getProj()
               << "; collation: " << cq->getQueryRequest().getCollation() << ")";

        return Status::OK();
    }

    // If query is not provided, make sure sort, projection, and collation are not in arguments.
    // We do not want to clear the entire cache inadvertently when the user
    // forgets to provide a value for "query".
    if (cmdObj.hasField("sort") || cmdObj.hasField("projection") || cmdObj.hasField("collation")) {
        return Status(ErrorCodes::BadValue,
                      "sort, projection, or collation provided without query");
    }

    planCache->clear();

    LOG(1) << ns << ": cleared plan cache";

    return Status::OK();
}

PlanCacheListPlans::PlanCacheListPlans()
    : PlanCacheCommand("planCacheListPlans",
                       "Displays the cached plans for a query shape.",
                       ActionType::planCacheRead) {}

Status PlanCacheListPlans::runPlanCacheCommand(OperationContext* txn,
                                               const std::string& ns,
                                               BSONObj& cmdObj,
                                               BSONObjBuilder* bob) {
    AutoGetCollectionForRead ctx(txn, ns);

    PlanCache* planCache;
    Status status = getPlanCache(txn, ctx.getCollection(), ns, &planCache);
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
    auto statusWithCQ = canonicalize(txn, ns, cmdObj);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

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
    unique_ptr<PlanCacheEntry> entry(entryRaw);

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
            Explain::statsToBSON(*stats, &statsBob);
        }
        statsBob.doneFast();
        reasonBob.doneFast();

        // BSON object for 'feedback' field shows scores from historical executions of the plan.
        BSONObjBuilder feedbackBob(planBob.subobjStart("feedback"));
        if (i == 0U) {
            feedbackBob.append("nfeedback", int(entry->feedback.size()));
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

}  // namespace mongo
