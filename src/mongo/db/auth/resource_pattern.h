/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/hash_namespace.h"

namespace mongo {

    /**
     * Representation of patterns that match various kinds of resources used in access control
     * checks.
     *
     * The resources are databases, collections and the the cluster itself.
     */
    class ResourcePattern {
    public:
        /**
         * Returns a pattern that matches absolutely any resource.
         */
        static ResourcePattern forAnyResource() {
            return ResourcePattern(matchAnyResource);
        }

        /**
         * Returns a pattern that matches any database or collection resource except collections for
         * which ns.isSystem().
         */
        static ResourcePattern forAnyNormalResource() {
            return ResourcePattern(matchAnyNormalResource);
        }

        /**
         * Returns a pattern that matches the "cluster" resource.
         */
        static ResourcePattern forClusterResource() {
            return ResourcePattern(matchClusterResource);
        }

        /**
         * Returns a pattern that matches the named database, and NamespaceStrings
         * "ns" for which ns.isSystem() is false and ns.db() == dbname.
         */
        static ResourcePattern forDatabaseName(const StringData& dbName) {
            return ResourcePattern(matchDatabaseName, NamespaceString(dbName, ""));
        }

        /**
         * Returns a pattern that matches NamespaceStrings "ns" for which ns.coll() ==
         * collectionName.
         */
        static ResourcePattern forCollectionName(const StringData& collectionName) {
            return ResourcePattern(matchCollectionName, NamespaceString("", collectionName));
        }

        /**
         * Returns a pattern that matches the given exact namespace string.
         */
        static ResourcePattern forExactNamespace(const NamespaceString& ns) {
            return ResourcePattern(matchExactNamespace, ns);
        }

        /**
         * Constructs a pattern that never matches.
         */
        ResourcePattern() : _matchType(matchNever) {}

        /**
         * Returns true if this pattern matches only exact namespaces.
         */
        bool isExactNamespacePattern() const {
            return _matchType == matchExactNamespace;
        }

        /**
         * Returns true if this pattern matches on the database name only.
         */
        bool isDatabasePattern() const {
            return _matchType == matchDatabaseName;
        }

        /**
         * Returns true if this pattern matches on the collection name only.
         */
        bool isCollectionPattern() const {
            return _matchType == matchCollectionName;
        }

        /**
         * Returns true if this pattern matches the cluster resource only.
         */
        bool isClusterResourcePattern() const {
            return _matchType == matchClusterResource;
        }

        /**
         * Returns true if this pattern matches only any normal resource.
         */
        bool isAnyNormalResourcePattern() const {
            return _matchType == matchAnyNormalResource;
        }

        /**
         * Returns true if this pattern matches nothing at all.
         */
        bool matchesNothing() const { return matchNever == _matchType; }

        /**
         * Returns true if this pattern matches every possible resource.
         */
        bool matchesEverything() const { return matchAnyResource == _matchType; }

        /**
         * Returns true if this pattern matches every resource except the cluster resource and
         * system collections.
         */
        bool matchesAnyNormalResource() const {
            return matchesEverything() || matchAnyNormalResource == _matchType;
        }

        /**
         * Returns true if this pattern matches the system resource.
         */
        bool matchesClusterResource() const {
            return matchesEverything() || _matchType == matchClusterResource;
        }

        /**
         * Returns true if this pattern matches the named database.
         */
        bool matchesDatabaseName(const StringData& dbName) const;

        /**
         * Returns true if this pattern matches the given NamespaceString.
         */
        bool matchesNamespaceString(const NamespaceString& ns) const;

        /**
         * Returns true if this pattern matches the target.
         *
         * The target must be a pattern matching some specific instance of a resource, rather than a
         * wildcard of some sort.  That is, it must be a database name pattern, an exact match
         * pattern, or a cluster resource pattern.
         *
         * As a special exception, the target may also be ResourcePattern::forAnyResource(), since
         * this is the target used for providing access control on applyOps and db.eval.  If the
         * target type is ResourcePattern::forAnyResource(), this method only returns true if this
         * instance is ResourcePattern::forAnyResource().
         *
         * Behavior for other patterns is undefined.
         */
        bool matchesResourcePattern(const ResourcePattern& target) const;

        /**
         * Returns the namespace that this pattern matches.
         *
         * Behavior is undefined unless isExactNamespacePattern() is true.
         */
        const NamespaceString& ns() const { return _ns; }

        /**
         * Returns the database that this pattern matches.
         *
         * Behavior is undefined unless the pattern is of type matchDatabaseName or
         * matchExactNamespace
         */
        StringData databaseToMatch() const { return _ns.db(); }

        /**
         * Returns the collection that this pattern matches.
         *
         * Behavior is undefined unless the pattern is of type matchCollectionName or
         * matchExactNamespace
         */
        StringData collectionToMatch() const { return _ns.coll(); }

        std::string toString() const;

        inline size_t hash() const  {
            // TODO: Choose a better hash function.
            return MONGO_HASH_NAMESPACE::hash<std::string>()(_ns.ns()) ^ _matchType;
        }

        bool operator==(const ResourcePattern& other) const {
            if (_matchType != other._matchType)
                return false;
            if (_ns != other._ns)
                return false;
            return true;
        }

    private:
        enum MatchType {
            matchNever = 0,  /// Matches no resource.
            matchClusterResource = 1,  /// Matches if the resource is the cluster resource.
            matchDatabaseName = 2,  /// Matches if the resource's database name is _ns.db().
            matchCollectionName = 3,  /// Matches if the resource's collection name is _ns.coll().
            matchExactNamespace = 4,  /// Matches if the resource's namespace name is _ns.
            matchAnyNormalResource = 5,  /// Matches all databases and non-system collections.
            matchAnyResource = 6  /// Matches absolutely anything.
        };

        explicit ResourcePattern(MatchType type) : _matchType(type) {}
        ResourcePattern(MatchType type, const NamespaceString& ns) : _matchType(type), _ns(ns) {}

        MatchType _matchType;
        NamespaceString _ns;
    };

    std::ostream& operator<<(std::ostream& os, const ResourcePattern& pattern);

}  // namespace mongo

MONGO_HASH_NAMESPACE_START
    template <> struct hash<mongo::ResourcePattern> {
        size_t operator()(const mongo::ResourcePattern& resource) const {
            return resource.hash();
        }
    };
MONGO_HASH_NAMESPACE_END
