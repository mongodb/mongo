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

#include "mongo/db/commands/plan_cache_commands.h"

#include "mongo/db/matcher/extensions_callback_real.h"

namespace mongo::plan_cache_commands {

StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                         StringData ns,
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
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString{ns});
    findCommand->setFilter(queryObj.getOwned());
    findCommand->setSort(sortObj.getOwned());
    findCommand->setProjection(projObj.getOwned());
    findCommand->setCollation(collationObj.getOwned());
    const ExtensionsCallbackReal extensionsCallback(
        opCtx, findCommand->getNamespaceOrUUID().nss().get_ptr());
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return std::move(statusWithCQ.getValue());
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

}  // namespace mongo::plan_cache_commands
