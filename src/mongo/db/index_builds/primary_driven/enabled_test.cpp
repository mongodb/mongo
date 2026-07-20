/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index_builds/primary_driven/enabled.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

namespace mongo::index_builds::primary_driven {
namespace {

constexpr bool kPrimaryDrivenIndexBuildsRequired = true;
constexpr bool kPrimaryDrivenIndexBuildsNotRequired = false;

class PrimaryDrivenIndexBuildTestProvider : public rss::StubPersistenceProvider {
public:
    explicit PrimaryDrivenIndexBuildTestProvider(bool mustUsePrimaryDrivenIndexBuilds)
        : _mustUse(mustUsePrimaryDrivenIndexBuilds) {}

    bool mustUsePrimaryDrivenIndexBuilds() const override {
        return _mustUse;
    }

private:
    bool _mustUse;
};

// Note: ScopedFCV and ScopedUninitializedFCV cannot be nested; both capture the prior FCV
// version in their constructor and assume it was initialized at that time.
class ScopedFCV {
public:
    explicit ScopedFCV(multiversion::FeatureCompatibilityVersion version)
        : _prev(serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion()) {
        serverGlobalParams.mutableFCV.setVersion(version);
    }
    ~ScopedFCV() {
        serverGlobalParams.mutableFCV.setVersion(_prev);
    }

private:
    multiversion::FeatureCompatibilityVersion _prev;
};

class ScopedUninitializedFCV {
public:
    ScopedUninitializedFCV()
        : _prev(serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion()) {
        serverGlobalParams.mutableFCV.reset();
    }
    ~ScopedUninitializedFCV() {
        serverGlobalParams.mutableFCV.setVersion(_prev);
    }

private:
    multiversion::FeatureCompatibilityVersion _prev;
};

class PrimaryDrivenIndexBuildEnabledTest : public ServiceContextTest {
public:
    void installProvider(bool mustUse) {
        rss::ReplicatedStorageService::get(getServiceContext())
            .setPersistenceProvider(std::make_unique<PrimaryDrivenIndexBuildTestProvider>(mustUse));
    }

    OperationContext* opCtx() {
        if (!_opCtx) {
            _opCtx = makeOperationContext();
        }
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(PrimaryDrivenIndexBuildEnabledTest, ProviderRequirementOverridesDisabledFlag) {
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", false);
    installProvider(kPrimaryDrivenIndexBuildsRequired);

    ASSERT_TRUE(enabled(opCtx()));
}

TEST_F(PrimaryDrivenIndexBuildEnabledTest, ProviderRequirementOverridesUninitializedFCV) {
    ScopedUninitializedFCV uninitFCV;
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", true);
    installProvider(kPrimaryDrivenIndexBuildsRequired);

    ASSERT_TRUE(enabled(opCtx()));
}

TEST_F(PrimaryDrivenIndexBuildEnabledTest, FallsBackToEnabledFlagWhenProviderDoesNotMandate) {
    // (Generic FCV reference): test usage
    ScopedFCV fcv(multiversion::GenericFCV::kLatest);
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", true);
    installProvider(kPrimaryDrivenIndexBuildsNotRequired);

    ASSERT_TRUE(enabled(opCtx()));
}

TEST_F(PrimaryDrivenIndexBuildEnabledTest, FallsBackToDisabledFlagWhenProviderDoesNotMandate) {
    // (Generic FCV reference): test usage
    ScopedFCV fcv(multiversion::GenericFCV::kLatest);
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", false);
    installProvider(kPrimaryDrivenIndexBuildsNotRequired);

    ASSERT_FALSE(enabled(opCtx()));
}

TEST_F(PrimaryDrivenIndexBuildEnabledTest, ToleratesUninitializedFCV) {
    ScopedUninitializedFCV uninitFCV;
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", true);
    installProvider(kPrimaryDrivenIndexBuildsNotRequired);

    ASSERT_FALSE(enabled(opCtx()));
}

TEST_F(PrimaryDrivenIndexBuildEnabledTest, FCVSnapshotOverloadHonorsCallerSnapshot) {
    // (Generic FCV reference): test usage
    ScopedFCV fcv(multiversion::GenericFCV::kLatest);
    unittest::ServerParameterGuard flag("featureFlagPrimaryDrivenIndexBuilds", true);
    installProvider(kPrimaryDrivenIndexBuildsNotRequired);

    const auto snapshotAtLatest = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ASSERT_TRUE(enabled(opCtx(), snapshotAtLatest));
}

}  // namespace
}  // namespace mongo::index_builds::primary_driven
