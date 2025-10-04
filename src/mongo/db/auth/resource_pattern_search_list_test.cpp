/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/resource_pattern_search_list.h"

#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::auth {
namespace {

struct TestCase {
    ResourcePattern target;
    std::set<ResourcePattern> lookups;
};

const auto kAdminDB = DatabaseName::createDatabaseName_forTest(boost::none, "admin"_sd);
const auto kTestDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);
const auto kTestFooNSS = NamespaceString::createNamespaceString_forTest(kTestDB, "foo"_sd);
const auto kTestViewsNSS =
    NamespaceString::createNamespaceString_forTest(kTestDB, "system.views"_sd);
const auto kTestBarBucketNSS =
    NamespaceString::createNamespaceString_forTest(kTestDB, "system.buckets.bar"_sd);
const auto kTestBarNSS = NamespaceString::createNamespaceString_forTest(kTestDB, "bar"_sd);

const auto kAnyRsrc = ResourcePattern::forAnyResource(boost::none);
const auto kAnyNormalRsrc = ResourcePattern::forAnyNormalResource(boost::none);
const auto kClusterRsrc = ResourcePattern::forClusterResource(boost::none);
const auto kAnySystemBuckets = ResourcePattern::forAnySystemBuckets(boost::none);

const auto kAdminDBRsrc = ResourcePattern::forDatabaseName(kAdminDB);
const auto kTestDBRsrc = ResourcePattern::forDatabaseName(kTestDB);
const auto kFooCollRsrc = ResourcePattern::forCollectionName(boost::none, "foo"_sd);
const auto kTestFooRsrc = ResourcePattern::forExactNamespace(kTestFooNSS);
const auto kViewsCollRsrc = ResourcePattern::forCollectionName(boost::none, "system.views"_sd);
const auto kTestViewsRsrc = ResourcePattern::forExactNamespace(kTestViewsNSS);
const auto kTestBarBucketRsrc = ResourcePattern::forExactNamespace(kTestBarBucketNSS);
const auto kTestBarSystemBucketsRsrc =
    ResourcePattern::forExactSystemBucketsCollection(kTestBarNSS);
const auto kTestDBSystemBucketsRsrc = ResourcePattern::forAnySystemBucketsInDatabase(kTestDB);
const auto kBarCollSystemBucketsRsrc =
    ResourcePattern::forAnySystemBucketsInAnyDatabase(boost::none, "bar"_sd);
const auto kBucketsBarCollRsrc =
    ResourcePattern::forCollectionName(boost::none, "system.buckets.bar"_sd);

const TestCase kTestCases[] = {
    {kAnyRsrc, {kAnyRsrc}},
    {kClusterRsrc, {kAnyRsrc, kClusterRsrc}},
    {kAdminDBRsrc, {kAnyRsrc, kAnyNormalRsrc, kAdminDBRsrc}},
    {kTestFooRsrc, {kAnyRsrc, kAnyNormalRsrc, kTestDBRsrc, kFooCollRsrc, kTestFooRsrc}},
    {kTestViewsRsrc, {kAnyRsrc, kTestDBRsrc, kViewsCollRsrc, kTestViewsRsrc}},
    {kTestBarBucketRsrc,
     {kAnyRsrc,
      kAnySystemBuckets,
      kTestBarSystemBucketsRsrc,
      kAnySystemBuckets,
      kTestDBSystemBucketsRsrc,
      kBarCollSystemBucketsRsrc,
      kBucketsBarCollRsrc,
      kTestBarBucketRsrc}},
};


TEST(ResourcePatternSearchListTest, ExpectedSearchLists) {
    for (const auto& testCase : kTestCases) {
        LOGV2(7705501, "Building search list", "target"_attr = testCase.target);
        const ResourcePatternSearchList searchList(testCase.target);
        for (auto it = searchList.cbegin(); it != searchList.cend(); ++it) {
            LOGV2(7705502, "Built search pattern", "search"_attr = *it);
            ASSERT_TRUE(testCase.lookups.find(*it) != testCase.lookups.end());
        }
    }
}

}  // namespace
}  // namespace mongo::auth
