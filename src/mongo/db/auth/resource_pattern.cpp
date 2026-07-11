// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/resource_pattern.h"

#include "mongo/db/namespace_string_util.h"


namespace mongo {

ResourcePattern::ResourcePattern(MatchTypeEnum type, const boost::optional<TenantId>& tenantId)
    : ResourcePattern(type,
                      tenantId ? NamespaceStringUtil::deserialize(
                                     tenantId, "", SerializationContext::stateDefault())
                               : NamespaceString()) {}

std::string ResourcePattern::serialize(const SerializationContext& context) const {
    switch (_matchType) {
        case MatchTypeEnum::kMatchNever:
            return "<no resources>";
        case MatchTypeEnum::kMatchClusterResource:
            return "<system resource>";
        case MatchTypeEnum::kMatchDatabaseName:
            return "<database " + DatabaseNameUtil::serialize(_ns.dbName(), context) + ">";
        case MatchTypeEnum::kMatchCollectionName:
            return "<collection " + std::string{_ns.coll()} + " in any database>";
        case MatchTypeEnum::kMatchExactNamespace:
            return "<" + NamespaceStringUtil::serialize(_ns, context) + ">";
        case MatchTypeEnum::kMatchAnyNormalResource:
            return "<all normal resources>";
        case MatchTypeEnum::kMatchAnyResource:
            return "<all resources>";
        case MatchTypeEnum::kMatchExactSystemBucketResource:
            return "<" + DatabaseNameUtil::serialize(_ns.dbName(), context) + ".system.bucket" +
                std::string{_ns.coll()} + " resources>";
        case MatchTypeEnum::kMatchSystemBucketInAnyDBResource:
            return "<any system.bucket." + std::string{_ns.coll()} + ">";
        case MatchTypeEnum::kMatchAnySystemBucketInDBResource:
            return "<" + DatabaseNameUtil::serialize(_ns.dbName(), context) + "system.bucket.*>";
        case MatchTypeEnum::kMatchAnySystemBucketResource:
            return "<any system.bucket resources>";
        default:
            return "<unknown resource pattern type>";
    }
}

std::string ResourcePattern::toString() const {
    return serialize();
}

std::ostream& operator<<(std::ostream& os, const ResourcePattern& pattern) {
    return os << pattern.toString();
}

}  // namespace mongo
