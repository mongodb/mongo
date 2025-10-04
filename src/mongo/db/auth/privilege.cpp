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

#include "mongo/db/auth/privilege.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
void uassertNoConflict(StringData resource, StringData found, bool cond) {
    uassert(ErrorCodes::BadValue,
            fmt::format("{} conflicts with resource type '{}'", resource, found),
            cond);
}

bool isValidCollectionName(StringData db, StringData coll) {
    if (NamespaceString::validCollectionName(coll)) {
        return true;
    }

    // local.oplog.$main is a real collection that the server will create. But, collection
    // names with a '$' character are illegal. We must make an exception for this collection
    // here so we can grant users access to it.
    if ((db == "local"_sd) && (coll == "oplog.$main"_sd)) {
        return true;
    }

    return false;
}
}  // namespace

void Privilege::addPrivilegeToPrivilegeVector(PrivilegeVector* privileges,
                                              const Privilege& privilegeToAdd) {
    for (PrivilegeVector::iterator it = privileges->begin(); it != privileges->end(); ++it) {
        if (it->getResourcePattern() == privilegeToAdd.getResourcePattern()) {
            it->addActions(privilegeToAdd.getActions());
            return;
        }
    }
    // No privilege exists yet for this resource
    privileges->push_back(privilegeToAdd);
}

void Privilege::addPrivilegesToPrivilegeVector(PrivilegeVector* privileges,
                                               const PrivilegeVector& privilegesToAdd) {
    for (auto&& priv : privilegesToAdd) {
        addPrivilegeToPrivilegeVector(privileges, priv);
    }
}

Privilege::Privilege(const ResourcePattern& resource, const ActionType action)
    : _resource(resource) {
    _actions.addAction(action);
}
Privilege::Privilege(const ResourcePattern& resource, ActionSet actions)
    : _resource(resource), _actions(std::move(actions)) {}

Privilege Privilege::resolvePrivilegeWithTenant(const boost::optional<TenantId>& tenantId,
                                                const auth::ParsedPrivilege& pp,
                                                std::vector<std::string>* unrecognizedActions) {
    using PR = auth::ParsedResource;
    const auto& rsrc = pp.getResource();
    Privilege ret;

    if (auto cluster = rsrc.getCluster()) {
        // { cluster: 1 }
        constexpr StringData kClusterRsrc = "resource: {cluster: true}"_sd;
        uassert(ErrorCodes::BadValue, "resource: {cluster: false} must be true", cluster.get());
        uassertNoConflict(kClusterRsrc, PR::kAnyResourceFieldName, !rsrc.getAnyResource());
        uassertNoConflict(kClusterRsrc, PR::kDbFieldName, !rsrc.getDb());
        uassertNoConflict(kClusterRsrc, PR::kCollectionFieldName, !rsrc.getCollection());
        uassertNoConflict(kClusterRsrc, PR::kSystemBucketsFieldName, !rsrc.getSystemBuckets());
        ret._resource = ResourcePattern::forClusterResource(tenantId);
    } else if (auto any = rsrc.getAnyResource()) {
        // { anyResource: 1 }
        constexpr StringData kAnyRsrc = "resource: {anyResource: true}"_sd;
        uassert(ErrorCodes::BadValue, "resource: {anyResource: false} must be true", any.get());
        uassertNoConflict(kAnyRsrc, PR::kDbFieldName, !rsrc.getDb());
        uassertNoConflict(kAnyRsrc, PR::kCollectionFieldName, !rsrc.getCollection());
        uassertNoConflict(kAnyRsrc, PR::kSystemBucketsFieldName, !rsrc.getSystemBuckets());
        ret._resource = ResourcePattern::forAnyResource(tenantId);
    } else {
        // db, collection, systemBuckets format
        const bool hasCollection = (rsrc.getCollection() != boost::none);
        const bool hasSystemBuckets = (rsrc.getSystemBuckets() != boost::none);
        uassertNoConflict("resource: {collection: '...'}",
                          PR::kSystemBucketsFieldName,
                          !(hasCollection && hasSystemBuckets));
        if (hasCollection) {
            // { db: '...', collection: '...' }
            uassert(ErrorCodes::BadValue,
                    "resource {collection: '...'} must include 'db' field as well",
                    rsrc.getDb());

            auto db = rsrc.getDb().get();
            auto coll = rsrc.getCollection().get();
            uassert(ErrorCodes::BadValue,
                    fmt::format("'{}' is not a valid collection name", coll),
                    coll.empty() || isValidCollectionName(db, coll));

            if (db.empty() && coll.empty()) {
                ret._resource = ResourcePattern::forAnyNormalResource(tenantId);
            } else if (db.empty()) {
                ret._resource = ResourcePattern::forCollectionName(tenantId, coll);
            } else if (coll.empty()) {
                ret._resource = ResourcePattern::forDatabaseName(
                    DatabaseNameUtil::deserialize(tenantId, db, rsrc.getSerializationContext()));
            } else {
                ret._resource = ResourcePattern::forExactNamespace(NamespaceStringUtil::deserialize(
                    tenantId, db, coll, SerializationContext::stateDefault()));
            }
        } else if (hasSystemBuckets) {
            // { systemBuckets: '...' }
            auto bucket = rsrc.getSystemBuckets().get();
            const bool emptyDb = !rsrc.getDb() || rsrc.getDb()->empty();
            if (emptyDb && bucket.empty()) {
                ret._resource = ResourcePattern::forAnySystemBuckets(tenantId);
            } else if (bucket.empty()) {
                ret._resource =
                    ResourcePattern::forAnySystemBucketsInDatabase(DatabaseNameUtil::deserialize(
                        tenantId, rsrc.getDb().get(), rsrc.getSerializationContext()));
            } else if (emptyDb) {
                ret._resource = ResourcePattern::forAnySystemBucketsInAnyDatabase(tenantId, bucket);
            } else {
                ret._resource = ResourcePattern::forExactSystemBucketsCollection(
                    NamespaceStringUtil::deserialize(tenantId,
                                                     rsrc.getDb().get(),
                                                     bucket,
                                                     SerializationContext::stateDefault()));
            }
        } else {
            uasserted(ErrorCodes::BadValue,
                      "resource pattern must contain 'collection' or 'systemBuckets' specifier");
        }
    }

    uassert(ErrorCodes::BadValue,
            "'actions' field of privilege resource must not be empty",
            !pp.getActions().empty());
    ret._actions = ActionSet::parseFromStringVector(pp.getActions(), unrecognizedActions);

    return ret;
}

PrivilegeVector Privilege::privilegeVectorFromParsedPrivilegeVector(
    const boost::optional<TenantId>& tenantId,
    const std::vector<auth::ParsedPrivilege>& parsedPrivileges,
    std::vector<std::string>* unrecognizedActions) {
    PrivilegeVector privileges;
    std::transform(parsedPrivileges.cbegin(),
                   parsedPrivileges.cend(),
                   std::back_inserter(privileges),
                   [&](const auto& pp) {
                       return Privilege::resolvePrivilegeWithTenant(
                           tenantId, pp, unrecognizedActions);
                   });
    return privileges;
}

void Privilege::addActions(const ActionSet& actionsToAdd) {
    _actions.addAllActionsFromSet(actionsToAdd);
}

void Privilege::removeActions(const ActionSet& actionsToRemove) {
    _actions.removeAllActionsFromSet(actionsToRemove);
}

bool Privilege::includesAction(const ActionType action) const {
    return _actions.contains(action);
}

bool Privilege::includesActions(const ActionSet& actions) const {
    return _actions.isSupersetOf(actions);
}

void Privilege::serialize(BSONObjBuilder* builder) const {
    toParsedPrivilege().serialize(builder);
}

BSONObj Privilege::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);
    return builder.obj();
}

void Privilege::serializePrivilegeVector(const PrivilegeVector& privileges,
                                         BSONArrayBuilder* builder) {
    for (const auto& currPriv : privileges) {
        BSONObjBuilder privBuilder(builder->subobjStart());
        currPriv.serialize(&privBuilder);
    }
}

auth::ParsedPrivilege Privilege::toParsedPrivilege() const {
    auth::ParsedPrivilege pp;
    pp.setActions(_actions.getActionsAsStringDatas());

    auth::ParsedResource rsrc;
    switch (_resource.matchType()) {
        case MatchTypeEnum::kMatchClusterResource:
            // { cluster: true }
            rsrc.setCluster(true);
            break;
        case MatchTypeEnum::kMatchAnyResource:
            // { anyResource: true }
            rsrc.setAnyResource(true);
            break;

        case MatchTypeEnum::kMatchExactNamespace:
            // { db: '...', collection: '...' }
            rsrc.setDb(_resource.dbNameToMatch().serializeWithoutTenantPrefix_UNSAFE());
            rsrc.setCollection(_resource.collectionToMatch());
            break;
        case MatchTypeEnum::kMatchDatabaseName:
            // { db: '...', collection: '' }
            rsrc.setDb(_resource.dbNameToMatch().serializeWithoutTenantPrefix_UNSAFE());
            rsrc.setCollection(""_sd);
            break;
        case MatchTypeEnum::kMatchCollectionName:
            // { db: '', collection: '...' }
            rsrc.setDb(""_sd);
            rsrc.setCollection(_resource.collectionToMatch());
            break;
        case MatchTypeEnum::kMatchAnyNormalResource:
            // { db: '', collection: '' }
            rsrc.setDb(""_sd);
            rsrc.setCollection(""_sd);
            break;

        case MatchTypeEnum::kMatchExactSystemBucketResource:
            // { db: '...', system_buckets: '...' }
            rsrc.setDb(_resource.dbNameToMatch().serializeWithoutTenantPrefix_UNSAFE());
            rsrc.setSystemBuckets(_resource.collectionToMatch());
            break;
        case MatchTypeEnum::kMatchSystemBucketInAnyDBResource:
            // { system_buckets: '...' }
            rsrc.setSystemBuckets(_resource.collectionToMatch());
            break;
        case MatchTypeEnum::kMatchAnySystemBucketInDBResource:
            // { db: '...', system_buckets: '' }
            rsrc.setDb(_resource.dbNameToMatch().serializeWithoutTenantPrefix_UNSAFE());
            rsrc.setSystemBuckets(""_sd);
            break;
        case MatchTypeEnum::kMatchAnySystemBucketResource:
            // { system_buckets: '' }
            rsrc.setSystemBuckets(""_sd);
            break;

        default:
            uasserted(ErrorCodes::InvalidOptions,
                      fmt::format("{} is not a valid user-grantable resource pattern",
                                  _resource.toString()));
    }

    pp.setResource(rsrc);

    return pp;
}

}  // namespace mongo
