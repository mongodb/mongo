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

#include <string>

#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    TEST(ResourcePattern, AnyResourcePattern) {
        ResourcePattern pattern = ResourcePattern::forAnyResource();
        ASSERT(pattern.matchesEverything());
        ASSERT(pattern.matchesAnyNormalResource());
        ASSERT(pattern.matchesClusterResource());
        ASSERT(pattern.matchesDatabaseName("admin"));
        ASSERT(pattern.matchesDatabaseName("config"));
        ASSERT(pattern.matchesDatabaseName("test"));
        ASSERT(pattern.matchesDatabaseName("work"));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, AnyNormalResourcePattern) {
        ResourcePattern pattern = ResourcePattern::forAnyNormalResource();
        ASSERT(!pattern.matchesEverything());
        ASSERT(pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(pattern.matchesDatabaseName("admin"));
        ASSERT(pattern.matchesDatabaseName("config"));
        ASSERT(pattern.matchesDatabaseName("test"));
        ASSERT(pattern.matchesDatabaseName("work"));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, ClusterResourcePattern) {
        ResourcePattern pattern = ResourcePattern::forClusterResource();
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, DatabaseNameTestPattern) {
        ResourcePattern pattern = ResourcePattern::forDatabaseName("test");
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));

        pattern = ResourcePattern::forDatabaseName("admin");
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, NormalCollectionPattern) {
        ResourcePattern pattern = ResourcePattern::forCollectionName("collection");
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, DottedCollectionPattern) {
        ResourcePattern pattern = ResourcePattern::forCollectionName("dotted.collection");
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("config")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, SystemCollectionPattern) {
        ResourcePattern pattern = ResourcePattern::forCollectionName("system.profile");
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

    TEST(ResourcePattern, ExactNamespacePattern) {
        ResourcePattern pattern = ResourcePattern::forExactNamespace(
                NamespaceString("admin.system.profile"));
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesAnyNormalResource());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));

        pattern = ResourcePattern::forExactNamespace(NamespaceString("test.system.profile"));
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));

        pattern = ResourcePattern::forExactNamespace(NamespaceString("test.collection"));
        ASSERT(!pattern.matchesEverything());
        ASSERT(!pattern.matchesClusterResource());
        ASSERT(!pattern.matchesDatabaseName("admin"));
        ASSERT(!pattern.matchesDatabaseName("config"));
        ASSERT(!pattern.matchesDatabaseName("test"));
        ASSERT(!pattern.matchesDatabaseName("work"));
        ASSERT(pattern.matchesNamespaceString(NamespaceString("test.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("test.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("work.system.frimfram")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.dotted.collection")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.not.system.profile")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.profile.not")));
        ASSERT(!pattern.matchesNamespaceString(NamespaceString("admin.system.frimfram")));
    }

}  // namespace
}  // namespace mongo
