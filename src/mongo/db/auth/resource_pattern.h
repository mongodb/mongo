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

#pragma once

#include <boost/optional.hpp>
#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"

namespace mongo {

/**
 * Representation of names of various kinds of resources targetable by the access control
 * system.
 *
 * Three of the types of name, "forDatabaseName", "forExactNamespace" and "forClusterResource",
 * can represent concrete resources targeted for manipulation by database operations.  All of
 * the types also act as patterns, useful for matching against groups of concrete resources as
 * part of the access control system.  See buildResourceSearchList() in
 * authorization_session.cpp for details.
 */
class ResourcePattern {
    friend class AuthorizationContract;

public:
    // TODO (SERVER-76195) Remove legacy non-tenant aware APIs from ResourcePattern
    // forAnyResource() - Remove boost::none default.
    // forAnyNormalResource() - Remove boost::none default.
    // forClusterResource() - Remove boost::none default.
    // forDatabaseName() - Remove `StringData` variant.
    // forCollectionName() - Remove variant without tenantId arg.
    // forAnySystemBuckets() - Remove boost::none default.
    // forAnySystemBucketsInDatabase() - Remove `StringData` variant.
    // forAnySystemBucketsInAnyDatabase() - Remove variant without tenantId arg.
    // forExactSystemBucketsCollection() - Remove variant with discrete db/coll args.
    // databaseToMatch() - Remove in favor of dbNameToMatch.

    /**
     * Returns a pattern that matches absolutely any resource.
     */
    static ResourcePattern forAnyResource(const boost::optional<TenantId>& tenantId = boost::none) {
        return ResourcePattern(MatchTypeEnum::kMatchAnyResource, tenantId);
    }

    /**
     * Returns a pattern that matches any database or collection resource except collections for
     * which ns.isSystem().
     */
    static ResourcePattern forAnyNormalResource(
        const boost::optional<TenantId>& tenantId = boost::none) {
        return ResourcePattern(MatchTypeEnum::kMatchAnyNormalResource, tenantId);
    }

    /**
     * Returns a pattern that matches the "cluster" resource.
     */
    static ResourcePattern forClusterResource(
        const boost::optional<TenantId>& tenantId = boost::none) {
        return ResourcePattern(MatchTypeEnum::kMatchClusterResource, tenantId);
    }

    /**
     * Returns a pattern that matches the named database, and NamespaceStrings
     * "ns" for which ns.isSystem() is false and ns.db() == dbname.
     */
    static ResourcePattern forDatabaseName(const DatabaseName& dbName) {
        return ResourcePattern(MatchTypeEnum::kMatchDatabaseName, NamespaceString(dbName));
    }

    static ResourcePattern forDatabaseName(StringData dbName) {
        return ResourcePattern(
            MatchTypeEnum::kMatchDatabaseName,
            NamespaceString::createNamespaceStringForAuth(boost::none, dbName, ""));
    }

    /**
     * Returns a pattern that matches NamespaceStrings "ns" for which ns.coll() ==
     * collectionName.
     */
    static ResourcePattern forCollectionName(const boost::optional<TenantId>& tenantId,
                                             StringData collectionName) {
        return ResourcePattern(
            MatchTypeEnum::kMatchCollectionName,
            NamespaceString::createNamespaceStringForAuth(tenantId, ""_sd, collectionName));
    }

    static ResourcePattern forCollectionName(StringData collectionName) {
        return forCollectionName(boost::none, collectionName);
    }

    /**
     * Returns a pattern that matches the given exact namespace string.
     */
    static ResourcePattern forExactNamespace(const NamespaceString& ns) {
        return ResourcePattern(MatchTypeEnum::kMatchExactNamespace, ns);
    }

    /**
     * Returns a pattern that matches any collection with the prefix "system.buckets." in any
     * database.
     */
    static ResourcePattern forAnySystemBuckets(
        const boost::optional<TenantId>& tenantId = boost::none) {
        return ResourcePattern(MatchTypeEnum::kMatchAnySystemBucketResource, tenantId);
    }

    /**
     * Returns a pattern that matches any collection with the prefix "system.buckets." in database
     * "db".
     */
    static ResourcePattern forAnySystemBucketsInDatabase(const DatabaseName& dbName) {
        return ResourcePattern(MatchTypeEnum::kMatchAnySystemBucketInDBResource,
                               NamespaceString(dbName));
    }

    static ResourcePattern forAnySystemBucketsInDatabase(StringData dbName) {
        return ResourcePattern(
            MatchTypeEnum::kMatchAnySystemBucketInDBResource,
            NamespaceString::createNamespaceStringForAuth(boost::none, dbName, ""));
    }

    /**
     * Returns a pattern that matches any collection with the prefix "system.buckets.<collection>"
     * in any database.
     */
    static ResourcePattern forAnySystemBucketsInAnyDatabase(
        const boost::optional<TenantId>& tenantId, StringData collectionName) {
        return ResourcePattern(
            MatchTypeEnum::kMatchSystemBucketInAnyDBResource,
            NamespaceString::createNamespaceStringForAuth(boost::none, "", collectionName));
    }

    static ResourcePattern forAnySystemBucketsInAnyDatabase(StringData collectionName) {
        return forAnySystemBucketsInAnyDatabase(boost::none, collectionName);
    }

    /**
     * Returns a pattern that matches a collection with the name
     * "<dbName>.system.buckets.<collectionName>"
     */
    static ResourcePattern forExactSystemBucketsCollection(const NamespaceString& nss) {
        invariant(!nss.coll().startsWith("system.buckets."));
        return ResourcePattern(MatchTypeEnum::kMatchExactSystemBucketResource, nss);
    }

    static ResourcePattern forExactSystemBucketsCollection(StringData dbName,
                                                           StringData collectionName) {
        return forExactSystemBucketsCollection(
            NamespaceString::createNamespaceStringForAuth(boost::none, dbName, collectionName));
    }

    /**
     * Constructs a pattern that never matches.
     */
    ResourcePattern() : _matchType(MatchTypeEnum::kMatchNever) {}

    /**
     * Returns true if this pattern matches only exact namespaces.
     */
    bool isExactNamespacePattern() const {
        return _matchType == MatchTypeEnum::kMatchExactNamespace;
    }

    /**
     * Returns true if this pattern matches on the database name only.
     */
    bool isDatabasePattern() const {
        return _matchType == MatchTypeEnum::kMatchDatabaseName;
    }

    /**
     * Returns true if this pattern matches on the collection name only.
     */
    bool isCollectionPattern() const {
        return _matchType == MatchTypeEnum::kMatchCollectionName;
    }

    /**
     * Returns true if this pattern matches the cluster resource only.
     */
    bool isClusterResourcePattern() const {
        return _matchType == MatchTypeEnum::kMatchClusterResource;
    }

    /**
     * Returns true if this pattern matches only any normal resource.
     */
    bool isAnyNormalResourcePattern() const {
        return _matchType == MatchTypeEnum::kMatchAnyNormalResource;
    }

    /**
     * Returns true if this pattern matches any resource.
     */
    bool isAnyResourcePattern() const {
        return _matchType == MatchTypeEnum::kMatchAnyResource;
    }

    /**
     * Returns true if this pattern matches a <db>.system.buckets.<collection name>.
     */
    bool isExactSystemBucketsCollection() const {
        return _matchType == MatchTypeEnum::kMatchExactSystemBucketResource;
    }

    /**
     * Returns true if this pattern matches a system.buckets.<collection name> in any db.
     */
    bool isAnySystemBucketsCollectionInAnyDB() const {
        return _matchType == MatchTypeEnum::kMatchSystemBucketInAnyDBResource;
    }

    /**
     * Returns true if this pattern matches a system.buckets.* in <db>.
     */
    bool isAnySystemBucketsCollectionInDB() const {
        return _matchType == MatchTypeEnum::kMatchAnySystemBucketInDBResource;
    }

    /**
     * Returns true if this pattern matches any collection prefixed with system.buckets
     */
    bool isAnySystemBucketsCollection() const {
        return _matchType == MatchTypeEnum::kMatchAnySystemBucketResource;
    }

    /**
     * Returns the namespace that this pattern matches.
     *
     * Behavior is undefined unless isExactNamespacePattern() is true.
     */
    const NamespaceString& ns() const {
        return _ns;
    }

    /**
     * Returns the tenantId that this pattern matches.
     */
    const boost::optional<TenantId>& tenantId() const {
        return _ns.tenantId();
    }

    /**
     * Returns the database that this pattern matches.
     *
     * Behavior is undefined unless the pattern is of type matchDatabaseName or
     * matchExactNamespace or matchExactSystemBucketResource or matchAnySystemBucketInDBResource
     */
    const DatabaseName& dbNameToMatch() const {
        return _ns.dbName();
    }

    StringData databaseToMatch() const {
        return _ns.db();
    }

    /**
     * Returns the collection that this pattern matches.
     *
     * Behavior is undefined unless the pattern is of type matchCollectionName or
     * matchExactNamespace or matchExactSystemBucketResource
     */
    StringData collectionToMatch() const {
        return _ns.coll();
    }

    std::string toString() const;

    bool operator==(const ResourcePattern& other) const {
        if (_matchType != other._matchType)
            return false;
        if (_ns != other._ns)
            return false;
        return true;
    }

    template <typename H>
    friend H AbslHashValue(H h, const ResourcePattern& rp) {
        return H::combine(std::move(h), rp._ns, rp._matchType);
    }

    /**
     * Returns a pattern for IDL generated code to use.
     */
    static ResourcePattern forAuthorizationContract(MatchTypeEnum e) {
        return ResourcePattern(e, boost::none);
    }

    // AuthorizationContract works directly with MatchTypeEnum. Users should not be concerned with
    // how a ResourcePattern was constructed.
    MatchTypeEnum matchType() const {
        return _matchType;
    }

private:
    ResourcePattern(MatchTypeEnum type, const boost::optional<TenantId>& tenantId)
        : ResourcePattern(type,
                          NamespaceString::createNamespaceStringForAuth(tenantId, ""_sd, ""_sd)) {}
    ResourcePattern(MatchTypeEnum type, const NamespaceString& ns) : _matchType(type), _ns(ns) {}

    MatchTypeEnum _matchType;
    NamespaceString _ns;
};

std::ostream& operator<<(std::ostream& os, const ResourcePattern& pattern);

}  // namespace mongo
