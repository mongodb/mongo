// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
using namespace std::literals::string_view_literals;

struct TestCase {
    ResourcePattern target;
    std::set<ResourcePattern> lookups;
};

const auto kAdminDB = DatabaseName::createDatabaseName_forTest(boost::none, "admin"sv);
const auto kTestDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"sv);
const auto kTestFooNSS = NamespaceString::createNamespaceString_forTest(kTestDB, "foo"sv);
const auto kTestViewsNSS =
    NamespaceString::createNamespaceString_forTest(kTestDB, "system.views"sv);
const auto kTestBarBucketNSS =
    NamespaceString::createNamespaceString_forTest(kTestDB, "system.buckets.bar"sv);
const auto kTestBarNSS = NamespaceString::createNamespaceString_forTest(kTestDB, "bar"sv);

const auto kAnyRsrc = ResourcePattern::forAnyResource(boost::none);
const auto kAnyNormalRsrc = ResourcePattern::forAnyNormalResource(boost::none);
const auto kClusterRsrc = ResourcePattern::forClusterResource(boost::none);
const auto kAnySystemBuckets = ResourcePattern::forAnySystemBuckets(boost::none);

const auto kAdminDBRsrc = ResourcePattern::forDatabaseName(kAdminDB);
const auto kTestDBRsrc = ResourcePattern::forDatabaseName(kTestDB);
const auto kFooCollRsrc = ResourcePattern::forCollectionName(boost::none, "foo"sv);
const auto kTestFooRsrc = ResourcePattern::forExactNamespace(kTestFooNSS);
const auto kViewsCollRsrc = ResourcePattern::forCollectionName(boost::none, "system.views"sv);
const auto kTestViewsRsrc = ResourcePattern::forExactNamespace(kTestViewsNSS);
const auto kTestBarBucketRsrc = ResourcePattern::forExactNamespace(kTestBarBucketNSS);
const auto kTestBarSystemBucketsRsrc =
    ResourcePattern::forExactSystemBucketsCollection(kTestBarNSS);
const auto kTestDBSystemBucketsRsrc = ResourcePattern::forAnySystemBucketsInDatabase(kTestDB);
const auto kBarCollSystemBucketsRsrc =
    ResourcePattern::forAnySystemBucketsInAnyDatabase(boost::none, "bar"sv);
const auto kBucketsBarCollRsrc =
    ResourcePattern::forCollectionName(boost::none, "system.buckets.bar"sv);

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
