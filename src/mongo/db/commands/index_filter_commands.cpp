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


#include "mongo/platform/basic.h"

#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace {

using std::string;
using std::vector;
using namespace mongo;

//
// Command instances.
// Registers commands with the command system and make commands
// available to the client.
//

MONGO_INITIALIZER_WITH_PREREQUISITES(SetupIndexFilterCommands, ())
(InitializerContext* context) {
    new ListFilters();
    new ClearFilters();
    new SetFilter();
}
}  // namespace

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;

IndexFilterCommand::IndexFilterCommand(const string& name, const string& helpText)
    : BasicCommand(name), helpText(helpText) {}

bool IndexFilterCommand::run(OperationContext* opCtx,
                             const string& dbname,
                             const BSONObj& cmdObj,
                             BSONObjBuilder& result) {
    const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
    AutoGetCollectionForReadCommand ctx(opCtx, nss);
    uassertStatusOK(runIndexFilterCommand(opCtx, ctx.getCollection(), cmdObj, &result));
    return true;
}

bool IndexFilterCommand::supportsWriteConcern(const BSONObj& cmd) const {
    return false;
}

Command::AllowedOnSecondary IndexFilterCommand::secondaryAllowed(ServiceContext*) const {
    return AllowedOnSecondary::kOptIn;
}

std::string IndexFilterCommand::help() const {
    return helpText;
}

Status IndexFilterCommand::checkAuthForCommand(Client* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) const {
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

Status ListFilters::runIndexFilterCommand(OperationContext* opCtx,
                                          const CollectionPtr& collection,
                                          const BSONObj& cmdObj,
                                          BSONObjBuilder* bob) {
    if (!collection) {
        // No collection - return empty array of filters.
        BSONArrayBuilder hintsBuilder(bob->subarrayStart("filters"));
        hintsBuilder.doneFast();
        return Status::OK();
    }

    QuerySettings* querySettings = QuerySettingsDecoration::get(collection->getSharedDecorations());
    invariant(querySettings);

    return list(*querySettings, bob);
}

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
    std::vector<AllowedIndexEntry> entries = querySettings.getAllAllowedIndices();
    for (vector<AllowedIndexEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i) {
        AllowedIndexEntry entry = *i;

        BSONObjBuilder hintBob(hintsBuilder.subobjStart());
        hintBob.append("query", entry.query);
        hintBob.append("sort", entry.sort);
        hintBob.append("projection", entry.projection);
        if (!entry.collation.isEmpty()) {
            hintBob.append("collation", entry.collation);
        }
        BSONArrayBuilder indexesBuilder(hintBob.subarrayStart("indexes"));
        for (BSONObjSet::const_iterator j = entry.indexKeyPatterns.begin();
             j != entry.indexKeyPatterns.end();
             ++j) {
            const BSONObj& index = *j;
            indexesBuilder.append(index);
        }
        for (const auto& indexEntry : entry.indexNames) {
            indexesBuilder.append(indexEntry);
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

Status ClearFilters::runIndexFilterCommand(OperationContext* opCtx,
                                           const CollectionPtr& collection,
                                           const BSONObj& cmdObj,
                                           BSONObjBuilder* bob) {
    if (!collection) {
        // No collection - do nothing.
        return Status::OK();
    }

    QuerySettings* querySettings = QuerySettingsDecoration::get(collection->getSharedDecorations());
    invariant(querySettings);

    PlanCache* planCacheClassic = CollectionQueryInfo::get(collection).getPlanCache();
    sbe::PlanCache* planCacheSBE = nullptr;
    invariant(planCacheClassic);

    if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        planCacheSBE = &sbe::getPlanCache(opCtx);
    }

    return clear(opCtx, collection, cmdObj, querySettings, planCacheClassic, planCacheSBE);
}

Status ClearFilters::clear(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           const BSONObj& cmdObj,
                           QuerySettings* querySettings,
                           PlanCache* planCacheClassic,
                           sbe::PlanCache* planCacheSBE) {
    // The planCacheClearFilters command runs in two modes:
    // - clear all index filters for the collection; or
    // - clear index filters for single query shape when a query shape is described in the
    //   command arguments.
    if (cmdObj.hasField("query")) {
        auto statusWithCQ = plan_cache_commands::canonicalize(opCtx, collection->ns().ns(), cmdObj);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }

        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        querySettings->removeAllowedIndices(cq->encodeKeyForPlanCacheCommand());

        stdx::unordered_set<uint32_t> planCacheCommandKeys({canonical_query_encoder::computeHash(
            canonical_query_encoder::encodeForPlanCacheCommand(*cq))});
        plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(planCacheCommandKeys,
                                                                          planCacheClassic);
        if (planCacheSBE) {
            plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(
                planCacheCommandKeys, collection->uuid(), planCacheSBE);
        }
        LOGV2(20479, "Removed index filter on query", "query"_attr = redact(cq->toStringShort()));

        return Status::OK();
    }

    // If query is not provided, make sure sort, projection, and collation are not in arguments. We
    // do not want to clear the entire cache inadvertently when the user forgot to provide a value
    // for "query".
    if (cmdObj.hasField("sort") || cmdObj.hasField("projection") || cmdObj.hasField("collation")) {
        return Status(ErrorCodes::BadValue,
                      "sort, projection, or collation provided without query");
    }

    // Get entries from query settings. We need to remove corresponding entries from the plan
    // cache shortly.
    std::vector<AllowedIndexEntry> entries = querySettings->getAllAllowedIndices();

    // OK to proceed with clearing all the index filters stored in 'QuerySettings'.
    querySettings->clearAllowedIndices();

    const NamespaceString nss(collection->ns());
    const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);

    // Remove corresponding entries from plan cache. Index filters affect the planning process
    // directly. If there were plans generated as a result of applying index filter, these need to
    // be invalidated. This allows the planner to re-populate the plan cache with non-filtered
    // indexed solutions next time the query is run. Resolve plan cache key from (query, sort,
    // projection, and user-defined collation) in query settings entry. Concurrency note: There's no
    // harm in removing plan cache entries one at a time. Only way that
    // removePlanCacheEntriesByPlanCacheCommandKeys() can fail is when the query shape has been
    // removed from the cache by some other means (re-index, collection info reset, ...). This is OK
    // since that's the intended effect of calling the
    // removePlanCacheEntriesByPlanCacheCommandKeys() function with the key from the index filter
    // entry.
    stdx::unordered_set<uint32_t> planCacheCommandKeys;
    for (auto entry : entries) {
        // Create canonical query.
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(entry.query);
        findCommand->setSort(entry.sort);
        findCommand->setProjection(entry.projection);
        findCommand->setCollation(entry.collation);
        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(findCommand),
                                         false,
                                         expCtx,
                                         extensionsCallback,
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        invariant(statusWithCQ.isOK());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        planCacheCommandKeys.insert(canonical_query_encoder::computeHash(
            canonical_query_encoder::encodeForPlanCacheCommand(*cq)));
    }
    plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(planCacheCommandKeys,
                                                                      planCacheClassic);
    if (planCacheSBE) {
        plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(
            planCacheCommandKeys, collection->uuid(), planCacheSBE);
    }

    LOGV2(20480,
          "Removed all index filters for collection",
          "namespace"_attr = collection->ns().ns());

    return Status::OK();
}

SetFilter::SetFilter()
    : IndexFilterCommand("planCacheSetFilter",
                         "Sets index filter for a query shape. Overrides existing filter.") {}

Status SetFilter::runIndexFilterCommand(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const BSONObj& cmdObj,
                                        BSONObjBuilder* bob) {
    if (!collection) {
        return Status(ErrorCodes::BadValue, "no such collection");
    }

    QuerySettings* querySettings = QuerySettingsDecoration::get(collection->getSharedDecorations());
    invariant(querySettings);

    PlanCache* planCacheClassic = CollectionQueryInfo::get(collection).getPlanCache();
    sbe::PlanCache* planCacheSBE = nullptr;
    invariant(planCacheClassic);

    if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        planCacheSBE = &sbe::getPlanCache(opCtx);
    }

    return set(opCtx, collection, cmdObj, querySettings, planCacheClassic, planCacheSBE);
}

Status SetFilter::set(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      const BSONObj& cmdObj,
                      QuerySettings* querySettings,
                      PlanCache* planCacheClassic,
                      sbe::PlanCache* planCacheSBE) {
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
    BSONObjSet indexes = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    stdx::unordered_set<std::string> indexNames;
    for (const auto& elt : indexesEltArray) {
        if (elt.type() == BSONType::Object) {
            BSONObj obj = elt.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "index specification cannot be empty");
            }
            indexes.insert(obj.getOwned());
        } else if (elt.type() == BSONType::String) {
            indexNames.insert(elt.String());
        } else {
            return Status(ErrorCodes::BadValue, "each item in indexes must be an object or string");
        }
    }

    auto statusWithCQ = plan_cache_commands::canonicalize(opCtx, collection->ns().ns(), cmdObj);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Add allowed indices to query settings, overriding any previous entries.
    querySettings->setAllowedIndices(*cq, indexes, indexNames);

    // Remove entries that match 'planCacheCommandKeys' from both plan caches.
    stdx::unordered_set<uint32_t> planCacheCommandKeys({canonical_query_encoder::computeHash(
        canonical_query_encoder::encodeForPlanCacheCommand(*cq))});
    plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(planCacheCommandKeys,
                                                                      planCacheClassic);
    if (planCacheSBE) {
        plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(
            planCacheCommandKeys, collection->uuid(), planCacheSBE);
    }

    LOGV2(20481,
          "Index filter set on query",
          "query"_attr = redact(cq->toStringShort()),
          "indexes"_attr = indexesElt);

    return Status::OK();
}

}  // namespace mongo
