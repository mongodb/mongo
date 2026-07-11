// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/plan_cache_commands.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <utility>


namespace mongo::plan_cache_commands {

StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                         const NamespaceString& nss,
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
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(queryObj.getOwned());
    findCommand->setSort(sortObj.getOwned());
    findCommand->setProjection(projObj.getOwned());
    findCommand->setCollation(collationObj.getOwned());

    tassert(ErrorCodes::BadValue,
            "Unsupported type UUID for namespace",
            findCommand->getNamespaceOrUUID().isNamespaceString());

    return CanonicalQuery::make(
        {.expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
         .parsedFind = ParsedFindCommandParams{
             .findCommand = std::move(findCommand),
             .extensionsCallback = ExtensionsCallbackReal(opCtx, &nss),
             .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
}

void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys, PlanCache* planCache) {
    planCache->removeIf(
        [&planCacheCommandKeys](const PlanCacheKey& key, const PlanCacheEntry& entry) {
            return planCacheCommandKeys.contains(entry.planCacheCommandKey);
        });
}

void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys,
    const UUID& collectionUuid,
    sbe::PlanCache* planCache) {
    planCache->removeIf([&](const sbe::PlanCacheKey& key, const sbe::PlanCacheEntry& entry) {
        return planCacheCommandKeys.contains(entry.planCacheCommandKey) &&
            key.getMainCollectionState().uuid == collectionUuid;
    });
}

Status validateNonEmptyCollation(const BSONObj& obj) {
    if (obj.isEmpty()) {
        return Status(ErrorCodes::BadValue, "optional field collation cannot be an empty object");
    }
    return Status::OK();
}
}  // namespace mongo::plan_cache_commands
