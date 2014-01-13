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
    Status getPlanCache(Database* db, const string& ns, PlanCache** planCacheOut) {
        invariant(db);

        Collection* collection = db->getCollection(ns);
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
        new PlanCacheDrop();
        new PlanCacheListPlans();

        return Status::OK();
    }

} // namespace

namespace mongo {

    using std::string;
    using std::stringstream;
    using std::vector;
    using boost::scoped_ptr;

    PlanCacheCommand::PlanCacheCommand(const string& name, const string& helpText,
                                       ActionType actionType)
        : Command(name),
          helpText(helpText),
          actionType(actionType) { }

    bool PlanCacheCommand::run(const string& dbname, BSONObj& cmdObj, int options,
                               string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        string ns = parseNs(dbname, cmdObj);

        Status status = runPlanCacheCommand(ns, cmdObj, &result);

        if (!status.isOK()) {
            addStatus(status, result);
            return false;
        }

        return true;
    }

    Command::LockType PlanCacheCommand::locktype() const {
        return NONE;
    }

    bool PlanCacheCommand::slaveOk() const {
        return false;
    }

    void PlanCacheCommand::help(stringstream& ss) const {
        ss << helpText << endl;
    }

    Status PlanCacheCommand::checkAuthForCommand(ClientBasic* client, const std::string& dbname,
                                                 const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = client->getAuthorizationSession();
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, actionType)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // static
    Status PlanCacheCommand::makeCacheKey(const string& ns, const BSONObj& cmdObj, PlanCacheKey* keyOut) {
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
        Status result = CanonicalQuery::canonicalize(ns, queryObj, sortObj, projObj, &cqRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        // Generate key
        PlanCacheKey key = cq->getPlanCacheKey();
        *keyOut = key;

        return Status::OK();
    }

    PlanCacheListQueryShapes::PlanCacheListQueryShapes() : PlanCacheCommand("planCacheListQueryShapes",
        "Displays all query shapes in a collection.",
        ActionType::planCacheRead) { }

    Status PlanCacheListQueryShapes::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                         BSONObjBuilder* bob) {
        // This is a read lock. The query cache is owned by the collection.
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return list(*planCache, bob);
    }

    // static
    Status PlanCacheListQueryShapes::list(const PlanCache& planCache, BSONObjBuilder* bob) {
        invariant(bob);

        // Fetch all cached solutions from plan cache.
        vector<CachedSolution*> solutions = planCache.getAllSolutions();

        BSONArrayBuilder arrayBuilder(bob->subarrayStart("shapes"));
        for (vector<CachedSolution*>::const_iterator i = solutions.begin(); i != solutions.end(); i++) {
            CachedSolution* cs = *i;
            invariant(cs);

            BSONObjBuilder shapeBuilder(arrayBuilder.subobjStart());
            shapeBuilder.append("query", cs->query);
            shapeBuilder.append("sort", cs->sort);
            shapeBuilder.append("projection", cs->projection);
            shapeBuilder.doneFast();

            // Release resources for cached solution after extracting query shape.
            delete cs;
        }
        arrayBuilder.doneFast();

        return Status::OK();
    }

    PlanCacheClear::PlanCacheClear() : PlanCacheCommand("planCacheClear",
        "Drops all cached queries in a collection.",
        ActionType::planCacheWrite) { }

    Status PlanCacheClear::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                               BSONObjBuilder* bob) {
        // This is a read lock. The query cache is owned by the collection.
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return clear(planCache);
    }

    // static
    Status PlanCacheClear::clear(PlanCache* planCache) {
        invariant(planCache);

        planCache->clear();

        return Status::OK();
    }

    PlanCacheDrop::PlanCacheDrop() : PlanCacheCommand("planCacheDrop",
        "Drops query shape from plan cache.",
        ActionType::planCacheWrite) { }

    Status PlanCacheDrop::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                              BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return drop(planCache, ns, cmdObj);
    }

    // static
    Status PlanCacheDrop::drop(PlanCache* planCache, const string& ns, const BSONObj& cmdObj) {
        PlanCacheKey key;
        Status status = makeCacheKey(ns, cmdObj, &key);
        if (!status.isOK()) {
            return status;
        }

        Status result = planCache->remove(key);
        if (!result.isOK()) {
            return result;
        }
        return Status::OK();
    }

    PlanCacheListPlans::PlanCacheListPlans() : PlanCacheCommand("planCacheListPlans",
        "Displays the cached plans for a query shape.",
        ActionType::planCacheRead) { }

    Status PlanCacheListPlans::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                   BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        PlanCache* planCache;
        Status status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return list(*planCache, ns, cmdObj, bob);
    }

    // static
    Status PlanCacheListPlans::list(const PlanCache& planCache, const std::string& ns,
                                    const BSONObj& cmdObj, BSONObjBuilder* bob) {
        PlanCacheKey key;
        Status status = makeCacheKey(ns, cmdObj, &key);
        if (!status.isOK()) {
            return status;
        }

        CachedSolution* crRaw;
        Status result = planCache.get(key, &crRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<CachedSolution> cr(crRaw);

        BSONArrayBuilder plansBuilder(bob->subarrayStart("plans"));
        size_t numPlans = cr->plannerData.size();
        for (size_t i = 0; i < numPlans; ++i) {
            BSONObjBuilder planBob(plansBuilder.subobjStart());

            // Create plan details field.
            // Currently, simple string representationg of
            // SolutionCacheData. Need to revisit format when we
            // need to parse user-provided plan details for planCacheAddPlan.
            SolutionCacheData* scd = cr->plannerData[i];
            BSONObjBuilder detailsBob(planBob.subobjStart("details"));
            detailsBob.append("solution", scd->toString());
            detailsBob.doneFast();

            // XXX: Fill in rest of fields with bogus data.
            // XXX: Fix these field values once we have fleshed out cache entries.
            planBob.append("reason", BSONObj());
            planBob.append("feedback", BSONObj());
        }
        plansBuilder.doneFast();

        return Status::OK();
    }

} // namespace mongo
