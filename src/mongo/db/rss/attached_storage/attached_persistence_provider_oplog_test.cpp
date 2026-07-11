// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(AttachedPersistenceProviderTest, SupportsPersistentOplogCapMaintainerThread) {
    rss::AttachedPersistenceProvider provider;
    ASSERT_TRUE(provider.supportsPersistentOplogCapMaintainerThread());
}

TEST(AttachedPersistenceProviderTest, SupportsAsyncOplogMarkerGenerationMatchesParameter) {
    rss::AttachedPersistenceProvider provider;
    {
        unittest::ServerParameterGuard disableAsync("oplogSamplingAsyncEnabled", false);
        ASSERT_FALSE(provider.supportsAsyncOplogMarkerGeneration());
    }
    {
        unittest::ServerParameterGuard enableAsync("oplogSamplingAsyncEnabled", true);
        ASSERT_TRUE(provider.supportsAsyncOplogMarkerGeneration());
    }
}

TEST(AttachedPersistenceProviderTest, SupportsOplogSampling) {
    rss::AttachedPersistenceProvider provider;
    ASSERT_TRUE(provider.supportsOplogSampling());
}

}  // namespace
}  // namespace mongo
