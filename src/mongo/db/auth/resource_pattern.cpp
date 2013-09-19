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

#include "mongo/platform/basic.h"

#include <iostream>

#include "mongo/db/auth/resource_pattern.h"

#include "mongo/util/log.h"

namespace mongo {

    bool ResourcePattern::matchesDatabaseName(const StringData& dbName) const {
        switch (_matchType) {
        case matchAnyResource: return true;
        case matchAnyNormalResource: return true;
        case matchDatabaseName: return dbName == _ns.db();
        default: return false;
        }
    }

    bool ResourcePattern::matchesNamespaceString(const NamespaceString& ns) const {
        switch (_matchType) {
        case matchAnyResource: return true;
        case matchAnyNormalResource: return !ns.isSystem();
        case matchDatabaseName: return ns.db() == _ns.db() && !ns.isSystem();
        case matchCollectionName: return ns.coll() == _ns.coll();
        case matchExactNamespace: return ns == _ns;
        default: return false;
        }
    }

    bool ResourcePattern::matchesResourcePattern(const ResourcePattern& target) const {
        switch (target._matchType) {
        case matchAnyResource:
            return matchesEverything();
        case matchClusterResource:
            return matchesClusterResource();
        case matchDatabaseName:
            return matchesDatabaseName(target._ns.db());
        case matchExactNamespace:
            return matchesNamespaceString(target._ns);
        default:
            error() << "Programming error: target of matchesResourcePattern() must be a specific "
                "resource or ResourcePattern::forAnyResource(); not a wildcard";
            fassertFailed(17136);
        }
    }

    std::string ResourcePattern::toString() const {
        switch (_matchType) {
        case matchNever:
            return "<no resources>";
        case matchClusterResource:
            return "<system resource>";
        case matchDatabaseName:
            return "<database " + _ns.db().toString() + ">";
        case matchCollectionName:
            return "<collection " + _ns.coll().toString() + " in any database>";
        case matchExactNamespace:
            return "<" + _ns.ns() + ">";
        case matchAnyNormalResource:
            return "<all normal resources>";
        case matchAnyResource:
            return "<all resources>";
        default:
            return "<unknown resource pattern type>";
        }
    }

    std::ostream& operator<<(std::ostream& os, const ResourcePattern& pattern) {
        return os << pattern.toString();
    }

}  // namespace mongo
