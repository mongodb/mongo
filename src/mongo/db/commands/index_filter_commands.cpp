/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include <sstream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/index_filter_commands.h"
#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/util/log.h"


namespace {

using std::string;
using std::vector;
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
 * Retrieves a collection's query settings and plan cache from the database.
 */
static Status getQuerySettingsAndPlanCache(OperationContext* txn,
                                           Collection* collection,
                                           const string& ns,
                                           QuerySettings** querySettingsOut,
                                           PlanCache** planCacheOut) {
    *querySettingsOut = NULL;
    *planCacheOut = NULL;
    if (NULL == collection) {
        return Status(ErrorCodes::BadValue, "no such collection");
    }

    CollectionInfoCache* infoCache = collection->infoCache();
    invariant(infoCache);

    QuerySettings* querySettings = infoCache->getQuerySettings();
    invariant(querySettings);

    *querySettingsOut = querySettings;

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

MONGO_INITIALIZER_WITH_PREREQUISITES(SetupIndexFilterCommands, MONGO_NO_PREREQUISITES)
(InitializerContext* context) {
    new ListFilters();
    new ClearFilters();
    new SetFilter();

    return Status::OK();
}

}  // namespace

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;
using std::unique_ptr;

IndexFilterCommand::IndexFilterCommand(const string& name, const string& helpText)
    : Command(name), helpText(helpText) {}

bool IndexFilterCommand::run(OperationContext* txn,
                             const string& dbname,
                             BSONObj& cmdObj,
                             int options,
                             string& errmsg,
                             BSONObjBuilder& result) {
    string ns = parseNs(dbname, cmdObj);

    Status status = runIndexFilterCommand(txn, ns, cmdObj, &result);

    if (!status.isOK()) {
        addStatus(status, result);
        return false;
    }

    return true;
}


bool IndexFilterCommand::supportsWriteConcern(const BSONObj& cmd) const {
    return false;
}

bool IndexFilterCommand::slaveOk() const {
    return false;
}

bool IndexFilterCommand::slaveOverrideOk() const {
    return true;
}

void IndexFilterCommand::help(stringstream& ss) const {
    ss << helpText;
}

Status IndexFilterCommand::checkAuthForCommand(ClientBasic* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

    if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::planCacheIndexFilter)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

ListFilters::ListFilters()
    : IndexFilterCommand("planCacheListFilters",
                         "Displays index filters for all query shapes in a collection.") {}

Status ListFilters::runIndexFilterCommand(OperationContext* txn,
                                          const string& ns,
                                          BSONObj& cmdObj,
                                          BSONObjBuilder* bob) {
    // This is a read lock. The query settings is owned by the collection.
    AutoGetCollectionForRead ctx(txn, ns);

    QuerySettings* querySettings;
    PlanCache* unused;
    Status status =
        getQuerySettingsAndPlanCache(txn, ctx.getCollection(), ns, &querySettings, &unused);
    if (!status.isOK()) {
        // No collection - return empty array of filters.
        BSONArrayBuilder hintsBuilder(bob->subarrayStart("filters"));
        hintsBuilder.doneFast();
        return Status::OK();
    }
    return list(*querySettings, bob);
}

// static
Status ListFilters::list(const QuerySettings& querySettings, BSONObjBuilder* bob) {
    invariant(bob);

    // Format of BSON result:
    //
    // {
    //     hints: [
    //         {
    //             query: <query>,
    //             sort: <sort>,
    //             projection: <projection>,
    //             indexes: [<index1>, <index2>, <index3>, ...]
    //         }
    //  }
    BSONArrayBuilder hintsBuilder(bob->subarrayStart("filters"));
    OwnedPointerVector<AllowedIndexEntry> entries;
    entries.mutableVector() = querySettings.getAllAllowedIndices();
    for (vector<AllowedIndexEntry*>::const_iterator i = entries.begin(); i != entries.end(); ++i) {
        AllowedIndexEntry* entry = *i;
        invariant(entry);

        BSONObjBuilder hintBob(hintsBuilder.subobjStart());
        hintBob.append("query", entry->query);
        hintBob.append("sort", entry->sort);
        hintBob.append("projection", entry->projection);
        if (!entry->collation.isEmpty()) {
            hintBob.append("collation", entry->collation);
        }
        BSONArrayBuilder indexesBuilder(hintBob.subarrayStart("indexes"));
        for (vector<BSONObj>::const_iterator j = entry->indexKeyPatterns.begin();
             j != entry->indexKeyPatterns.end();
             ++j) {
            const BSONObj& index = *j;
            indexesBuilder.append(index);
        }
        indexesBuilder.doneFast();
    }
    hintsBuilder.doneFast();
    return Status::OK();
}

ClearFilters::ClearFilters()
    : IndexFilterCommand("planCacheClearFilters",
                         "Clears index filter for a single query shape or, "
                         "if the query shape is omitted, all filters for the collection.") {}

Status ClearFilters::runIndexFilterCommand(OperationContext* txn,
                                           const std::string& ns,
                                           BSONObj& cmdObj,
                                           BSONObjBuilder* bob) {
    // This is a read lock. The query settings is owned by the collection.
    AutoGetCollectionForRead ctx(txn, ns);

    QuerySettings* querySettings;
    PlanCache* planCache;
    Status status =
        getQuerySettingsAndPlanCache(txn, ctx.getCollection(), ns, &querySettings, &planCache);
    if (!status.isOK()) {
        // No collection - do nothing.
        return Status::OK();
    }
    return clear(txn, querySettings, planCache, ns, cmdObj);
}

// static
Status ClearFilters::clear(OperationContext* txn,
                           QuerySettings* querySettings,
                           PlanCache* planCache,
                           const std::string& ns,
                           const BSONObj& cmdObj) {
    invariant(querySettings);

    // According to the specification, the planCacheClearFilters command runs in two modes:
    // - clear all hints; or
    // - clear hints for single query shape when a query shape is described in the
    //   command arguments.
    if (cmdObj.hasField("query")) {
        auto statusWithCQ = PlanCacheCommand::canonicalize(txn, ns, cmdObj);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }

        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        querySettings->removeAllowedIndices(planCache->computeKey(*cq));

        // Remove entry from plan cache
        planCache->remove(*cq);

        LOG(0) << "Removed index filter on " << ns << " " << cq->toStringShort();

        return Status::OK();
    }

    // If query is not provided, make sure sort, projection, and collation are not in arguments.
    // We do not want to clear the entire cache inadvertently when the user
    // forgot to provide a value for "query".
    if (cmdObj.hasField("sort") || cmdObj.hasField("projection") || cmdObj.hasField("collation")) {
        return Status(ErrorCodes::BadValue,
                      "sort, projection, or collation provided without query");
    }

    // Get entries from query settings. We need to remove corresponding entries from the plan
    // cache shortly.
    OwnedPointerVector<AllowedIndexEntry> entries;
    entries.mutableVector() = querySettings->getAllAllowedIndices();

    // OK to proceed with clearing entire cache.
    querySettings->clearAllowedIndices();

    const NamespaceString nss(ns);
    const ExtensionsCallbackReal extensionsCallback(txn, &nss);

    // Remove corresponding entries from plan cache.
    // Admin hints affect the planning process directly. If there were
    // plans generated as a result of applying index filter, these need to be
    // invalidated. This allows the planner to re-populate the plan cache with
    // non-filtered indexed solutions next time the query is run.
    // Resolve plan cache key from (query, sort, projection) in query settings entry.
    // Concurrency note: There's no harm in removing plan cache entries one at at time.
    // Only way that PlanCache::remove() can fail is when the query shape has been removed from
    // the cache by some other means (re-index, collection info reset, ...). This is OK since
    // that's the intended effect of calling the remove() function with the key from the hint entry.
    for (vector<AllowedIndexEntry*>::const_iterator i = entries.begin(); i != entries.end(); ++i) {
        AllowedIndexEntry* entry = *i;
        invariant(entry);

        // Create canonical query.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(entry->query);
        qr->setSort(entry->sort);
        qr->setProj(entry->projection);
        qr->setCollation(entry->collation);
        auto statusWithCQ = CanonicalQuery::canonicalize(txn, std::move(qr), extensionsCallback);
        invariantOK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Remove plan cache entry.
        planCache->remove(*cq);
    }

    LOG(0) << "Removed all index filters for collection: " << ns;

    return Status::OK();
}

SetFilter::SetFilter()
    : IndexFilterCommand("planCacheSetFilter",
                         "Sets index filter for a query shape. Overrides existing filter.") {}

Status SetFilter::runIndexFilterCommand(OperationContext* txn,
                                        const std::string& ns,
                                        BSONObj& cmdObj,
                                        BSONObjBuilder* bob) {
    // This is a read lock. The query settings is owned by the collection.
    const NamespaceString nss(ns);
    AutoGetCollectionForRead ctx(txn, nss);

    QuerySettings* querySettings;
    PlanCache* planCache;
    Status status =
        getQuerySettingsAndPlanCache(txn, ctx.getCollection(), ns, &querySettings, &planCache);
    if (!status.isOK()) {
        return status;
    }
    return set(txn, querySettings, planCache, ns, cmdObj);
}

// static
Status SetFilter::set(OperationContext* txn,
                      QuerySettings* querySettings,
                      PlanCache* planCache,
                      const string& ns,
                      const BSONObj& cmdObj) {
    // indexes - required
    BSONElement indexesElt = cmdObj.getField("indexes");
    if (indexesElt.eoo()) {
        return Status(ErrorCodes::BadValue, "required field indexes missing");
    }
    if (indexesElt.type() != mongo::Array) {
        return Status(ErrorCodes::BadValue, "required field indexes must be an array");
    }
    vector<BSONElement> indexesEltArray = indexesElt.Array();
    if (indexesEltArray.empty()) {
        return Status(ErrorCodes::BadValue,
                      "required field indexes must contain at least one index");
    }
    vector<BSONObj> indexes;
    for (vector<BSONElement>::const_iterator i = indexesEltArray.begin();
         i != indexesEltArray.end();
         ++i) {
        const BSONElement& elt = *i;
        if (!elt.isABSONObj()) {
            return Status(ErrorCodes::BadValue, "each item in indexes must be an object");
        }
        BSONObj obj = elt.Obj();
        if (obj.isEmpty()) {
            return Status(ErrorCodes::BadValue, "index specification cannot be empty");
        }
        indexes.push_back(obj.getOwned());
    }

    auto statusWithCQ = PlanCacheCommand::canonicalize(txn, ns, cmdObj);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Add allowed indices to query settings, overriding any previous entries.
    querySettings->setAllowedIndices(*cq, planCache->computeKey(*cq), indexes);

    // Remove entry from plan cache.
    planCache->remove(*cq);

    LOG(0) << "Index filter set on " << ns << " " << cq->toStringShort() << " " << indexesElt;

    return Status::OK();
}

}  // namespace mongo
