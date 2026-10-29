// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/app_name_exemption_matcher.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>

namespace mongo::admission {
namespace {
using namespace std::literals::string_view_literals;

class AppNameExemptionMatcherTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();

        _client = getServiceContext()->getService()->makeClient("TestClient");
        _opCtx = _client->makeOperationContext();
        _matcher = std::make_unique<AppNameExemptionMatcher>(_exemptions);
    }

    void tearDown() override {
        _matcher.reset();
        _opCtx.reset();
        _client.reset();
        ServiceContextTest::tearDown();
    }

    void setClientMetadata(std::string_view driverName, std::string_view appName) {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serialize(driverName, "1.0.0"sv, appName, &builder));
        auto doc = builder.obj();
        ClientMetadata::setFromMetadata(_client.get(), doc.firstElement(), false);
    }

    void setExemptions(std::vector<std::string> exemptions) {
        _exemptions.update(std::make_shared<std::vector<std::string>>(std::move(exemptions)));
    }

    bool isExempted() {
        return _matcher->isExempted(_client.get());
    }

    VersionedValue<std::vector<std::string>> _exemptions{
        std::make_shared<std::vector<std::string>>()};
    std::unique_ptr<AppNameExemptionMatcher> _matcher;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(AppNameExemptionMatcherTest, NoMetadataNotExempted) {
    setExemptions({"TestApp"});
    ASSERT_FALSE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, EmptyExemptionListNotExempted) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({});
    ASSERT_FALSE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, ExactAppNameMatch) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"TestApp"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, ExactDriverNameMatch) {
    setClientMetadata("TestDriver", "SomeApp");
    setExemptions({"TestDriver"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, AppNamePrefixMatch) {
    setClientMetadata("SomeDriver", "MongoDB Compass Community");
    setExemptions({"MongoDB Compass"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, DriverNamePrefixMatch) {
    setClientMetadata("nodejs-mongodb-driver", "MyApp");
    setExemptions({"nodejs"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, NoMatch) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"OtherApp", "OtherDriver"});
    ASSERT_FALSE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, MultipleExemptions) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"OtherApp", "TestApp", "AnotherApp"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, CaseSensitiveMatch) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"testapp"});
    ASSERT_FALSE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, ExemptionLongerThanAppName) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"TestAppExtended"});
    ASSERT_FALSE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, EmptyStringExemptionMatchesAll) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({""});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, BothAppAndDriverMatch) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"Test"});
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, CacheIsReused) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"TestApp"});
    ASSERT_TRUE(isExempted());
    ASSERT_TRUE(isExempted());
}

TEST_F(AppNameExemptionMatcherTest, CacheInvalidatesOnExemptionListChange) {
    setClientMetadata("TestDriver", "TestApp");
    setExemptions({"OtherApp"});
    ASSERT_FALSE(isExempted());

    setExemptions({"TestApp"});
    ASSERT_TRUE(isExempted());

    setExemptions({});
    ASSERT_FALSE(isExempted());
}

}  // namespace
}  // namespace mongo::admission
