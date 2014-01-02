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
#include "mongo/db/database.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/structure/collection.h"

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
     * Retrieves a collection's plan cache from the client context.
     */
    Status getPlanCache(const Client::Context& ctx, PlanCache** planCacheOut) {
        const char* ns = ctx.ns();

        Database* db = ctx.db();
        verify(db);

        Collection* collection = db->getCollection(ns);
        if (NULL == collection) {
            return Status(ErrorCodes::BadValue, "no such collection");
        }

        CollectionInfoCache* infoCache = collection->infoCache();
        verify(infoCache);

        PlanCache* planCache = infoCache->getPlanCache();
        verify(planCache);

        *planCacheOut = planCache;
        return Status::OK();
    }

    Status getPlanCache(Client::ReadContext& readCtx, PlanCache** planCacheOut) {
        Client::Context& ctx = readCtx.ctx();
        return getPlanCache(ctx, planCacheOut);
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
        new PlanCacheListKeys();
        new PlanCacheClear();
        new PlanCacheGenerateKey();
        new PlanCacheGet();
        new PlanCacheDrop();
        new PlanCacheListPlans();
        new PlanCachePinPlan();
        new PlanCacheUnpinPlan();
        new PlanCacheAddPlan();
        new PlanCacheShunPlan();

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

    PlanCacheListKeys::PlanCacheListKeys() : PlanCacheCommand("planCacheListKeys",
        "Displays keys of all cached queries in a collection.",
        ActionType::planCacheRead) { }

    Status PlanCacheListKeys::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                  BSONObjBuilder* bob) {
        // This is a read lock. The query cache is owned by the collection.
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return listKeys(*planCache, bob);
    }

    // static
    Status PlanCacheListKeys::listKeys(const PlanCache& planCache, BSONObjBuilder* bob) {
        verify(bob);

        vector<PlanCacheKey> keys;
        planCache.getKeys(&keys);

        BSONArrayBuilder arrayBuilder(bob->subarrayStart("queries"));
        for (vector<PlanCacheKey>::const_iterator i = keys.begin(); i != keys.end(); i++) {
            const PlanCacheKey& key = *i;
            arrayBuilder.append(key);
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
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return clear(planCache);
    }

    // static
    Status PlanCacheClear::clear(PlanCache* planCache) {
        verify(planCache);

        planCache->clear();

        return Status::OK();
    }

    PlanCacheGenerateKey::PlanCacheGenerateKey() : PlanCacheCommand("planCacheGenerateKey",
        "Returns a key into the cache for a query. Similar queries with the same query shape "
        "will resolve to the same key.",
        ActionType::planCacheRead) { }

    Status PlanCacheGenerateKey::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                     BSONObjBuilder* bob) {
        return generate(ns, cmdObj, bob);
    }

    // static
    Status PlanCacheGenerateKey::generate(const string& ns, const BSONObj& cmdObj,
                                          BSONObjBuilder* bob) {
        verify(bob);

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

        // Create canonical query
        BSONObj projObj;
        CanonicalQuery* cqRaw;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, sortObj, projObj, &cqRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        // Canonical query needs to be normalized before generating cache key.
        normalizeQueryForCache(cq.get());

        // Generate key
        PlanCacheKey key = getPlanCacheKey(*cq);
        bob->append("key", key);

        return Status::OK();
    }

    PlanCacheGet::PlanCacheGet() : PlanCacheCommand("planCacheGet",
        "Looks up the query shape, sort order and projection using a cache key.",
        ActionType::planCacheRead) { }

    Status PlanCacheGet::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                             BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return get(*planCache, cmdObj, bob);
    }

    // static
    Status PlanCacheGet::get(const PlanCache& planCache, const BSONObj& cmdObj,
                             BSONObjBuilder* bob) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        CachedSolution* crRaw;
        Status result = planCache.get(key, &crRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<CachedSolution> cr(crRaw);

        // XXX: Fix these field values once we have fleshed out cache entries.
        bob->append("query", cr->query);
        bob->append("sort", cr->sort);
        bob->append("projection", cr->projection);

        return Status::OK();
    }

    PlanCacheDrop::PlanCacheDrop() : PlanCacheCommand("planCacheDrop",
        "Drops using a cache key.",
        ActionType::planCacheWrite) { }

    Status PlanCacheDrop::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                              BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return drop(planCache, cmdObj);
    }

    // static
    Status PlanCacheDrop::drop(PlanCache* planCache, const BSONObj& cmdObj) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();
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
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return list(*planCache, cmdObj, bob);
    }

    // static
    Status PlanCacheListPlans::list(const PlanCache& planCache, const BSONObj& cmdObj,
                                    BSONObjBuilder* bob) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        CachedSolution* crRaw;
        Status result = planCache.get(key, &crRaw);
        if (!result.isOK()) {
            return result;
        }
        scoped_ptr<CachedSolution> cr(crRaw);

        // XXX: Fix these field values once we have fleshed out cache entries.
        BSONArrayBuilder plansBuilder(bob->subarrayStart("plans"));
        size_t numPlans = cr->numPlans;
        for (size_t i = 0; i < numPlans; ++i) {
            BSONObjBuilder planBob(plansBuilder.subobjStart());
            stringstream ss;
            ss << "plan" << i;
            planBob.append("plan", ss.str());
        }
        plansBuilder.doneFast();

        return Status::OK();
    }

    PlanCachePinPlan::PlanCachePinPlan() : PlanCacheCommand("planCachePinPlan",
        "This command allows the user to pin a plan so that "
        "it will always be used for query execution.",
        ActionType::planCacheWrite) { }

    Status PlanCachePinPlan::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                 BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return pin(planCache, cmdObj);
    }

    // static
    Status PlanCachePinPlan::pin(PlanCache* planCache, const BSONObj& cmdObj) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        BSONElement planElt = cmdObj.getField("plan");
        if (planElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field plan missing");
        }
        if (mongo::String != planElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field plan must be a string");
        }
        PlanID plan = planElt.String();

        Status result = planCache->pin(key, plan);
        if (!result.isOK()) {
            return result;
        }

        return Status::OK();
    }

    PlanCacheUnpinPlan::PlanCacheUnpinPlan() : PlanCacheCommand("planCacheUnpinPlan",
        "This command allows the user to unpin any plan that might be pinned to a query.",
        ActionType::planCacheWrite) { }

    Status PlanCacheUnpinPlan::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                   BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return unpin(planCache, cmdObj);
    }

    // static
    Status PlanCacheUnpinPlan::unpin(PlanCache* planCache, const BSONObj& cmdObj) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        Status result = planCache->unpin(key);
        if (!result.isOK()) {
            return result;
        }

        return Status::OK();
    }

    PlanCacheAddPlan::PlanCacheAddPlan() : PlanCacheCommand("planCacheAddPlan",
        "Adds a user-defined plan to an existing query in the cache.",
        ActionType::planCacheWrite) { }

    Status PlanCacheAddPlan::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                 BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return add(planCache, cmdObj, bob);
    }

    // static
    Status PlanCacheAddPlan::add(PlanCache* planCache, const BSONObj& cmdObj,
                                 BSONObjBuilder* bob) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        BSONElement detailsElt = cmdObj.getField("details");
        if (detailsElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field details missing");
        }
        if (!detailsElt.isABSONObj()) {
            return Status(ErrorCodes::BadValue,
                          "required field details must be an object");
        }
        BSONObj details = detailsElt.Obj();

        PlanID plan;
        Status result = planCache->addPlan(key, details, &plan);
        if (!result.isOK()) {
            return result;
        }

        bob->append("plan", plan);
        return Status::OK();
    }

    PlanCacheShunPlan::PlanCacheShunPlan() : PlanCacheCommand("planCacheShunPlan",
        "Marks a plan as non-executable. This takes the plan out of consideration "
        "in the plan selection for query execution.",
        ActionType::planCacheWrite) { }

    Status PlanCacheShunPlan::runPlanCacheCommand(const string& ns, BSONObj& cmdObj,
                                                  BSONObjBuilder* bob) {
        Client::ReadContext readCtx(ns);
        PlanCache* planCache;
        Status status = getPlanCache(readCtx, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return shun(planCache, cmdObj);
    }

    // static
    Status PlanCacheShunPlan::shun(PlanCache* planCache, const BSONObj& cmdObj) {
        BSONElement keyElt = cmdObj.getField("key");
        if (keyElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field key missing");
        }
        // This has to be kept in sync with actual type of PlanCacheKey
        if (mongo::String != keyElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field key must be compatible with cache key type");
        }
        PlanCacheKey key = keyElt.String();

        BSONElement planElt = cmdObj.getField("plan");
        if (planElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field plan missing");
        }
        if (mongo::String != planElt.type()) {
            return Status(ErrorCodes::BadValue,
                          "required field plan must be a string");
        }
        PlanID plan = planElt.String();

        Status result = planCache->shunPlan(key, plan);
        if (!result.isOK()) {
            return result;
        }

        return Status::OK();
    }

} // namespace mongo
