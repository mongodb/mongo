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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/hash_namespace.h"

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
    static ResourcePattern forDatabaseName(StringData dbName) {
        return ResourcePattern(matchDatabaseName, NamespaceString(dbName, ""));
    }

    /**
     * Returns a pattern that matches NamespaceStrings "ns" for which ns.coll() ==
     * collectionName.
     */
    static ResourcePattern forCollectionName(StringData collectionName) {
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
     * Returns true if this pattern matches any resource.
     */
    bool isAnyResourcePattern() const {
        return _matchType == matchAnyResource;
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
     * Returns the database that this pattern matches.
     *
     * Behavior is undefined unless the pattern is of type matchDatabaseName or
     * matchExactNamespace
     */
    StringData databaseToMatch() const {
        return _ns.db();
    }

    /**
     * Returns the collection that this pattern matches.
     *
     * Behavior is undefined unless the pattern is of type matchCollectionName or
     * matchExactNamespace
     */
    StringData collectionToMatch() const {
        return _ns.coll();
    }

    std::string toString() const;

    inline size_t hash() const {
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
        matchNever = 0,              /// Matches no resource.
        matchClusterResource = 1,    /// Matches if the resource is the cluster resource.
        matchDatabaseName = 2,       /// Matches if the resource's database name is _ns.db().
        matchCollectionName = 3,     /// Matches if the resource's collection name is _ns.coll().
        matchExactNamespace = 4,     /// Matches if the resource's namespace name is _ns.
        matchAnyNormalResource = 5,  /// Matches all databases and non-system collections.
        matchAnyResource = 6         /// Matches absolutely anything.
    };

    explicit ResourcePattern(MatchType type) : _matchType(type) {}
    ResourcePattern(MatchType type, const NamespaceString& ns) : _matchType(type), _ns(ns) {}

    MatchType _matchType;
    NamespaceString _ns;
};

std::ostream& operator<<(std::ostream& os, const ResourcePattern& pattern);

}  // namespace mongo

MONGO_HASH_NAMESPACE_START
template <>
struct hash<mongo::ResourcePattern> {
    size_t operator()(const mongo::ResourcePattern& resource) const {
        return resource.hash();
    }
};
MONGO_HASH_NAMESPACE_END
