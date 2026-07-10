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

#include "mongo/rpc/metadata/client_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/metadata/client_metadata_server_parameters_gen.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
using namespace std::literals::string_view_literals;

constexpr auto kMetadataDoc = "client"sv;
constexpr auto kApplication = "application"sv;
constexpr auto kDriver = "driver"sv;
constexpr auto kName = "name"sv;
constexpr auto kPid = "pid"sv;
constexpr auto kType = "type"sv;
constexpr auto kVersion = "version"sv;
constexpr auto kOperatingSystem = "os"sv;
constexpr auto kArchitecture = "architecture"sv;
constexpr auto kMongos = "mongos"sv;
constexpr auto kClient = "client"sv;
constexpr auto kHost = "host"sv;
constexpr auto kCid = "cid"sv;

constexpr auto kUnknown = "unkown"sv;

#define ASSERT_DOC_OK(...)                                                                \
    do {                                                                                  \
        auto _swParseStatus =                                                             \
            ClientMetadata::parse(BSON(kMetadataDoc << BSON(__VA_ARGS__))[kMetadataDoc]); \
        ASSERT_OK(_swParseStatus.getStatus());                                            \
    } while (0)

#define ASSERT_DOC_NOT_OK(...)                                                            \
    do {                                                                                  \
        auto _swParseStatus =                                                             \
            ClientMetadata::parse(BSON(kMetadataDoc << BSON(__VA_ARGS__))[kMetadataDoc]); \
        ASSERT_NOT_OK(_swParseStatus.getStatus());                                        \
    } while (0)


TEST(ClientMetadataTest, TestLoopbackTest) {
    // serializePrivate with appName
    {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", "g", &builder));

        auto obj = builder.obj();
        auto swParseStatus = ClientMetadata::parse(obj[kMetadataDoc]);
        ASSERT_OK(swParseStatus.getStatus());
        ASSERT_EQUALS("g", swParseStatus.getValue().value().getApplicationName());

        auto pid = ProcessId::getCurrent().toString();

        using BOB = BSONObjBuilder;
        BSONObj outDoc =
            BOB{}
                .append(kMetadataDoc,
                        BOB{}
                            .append(kApplication,
                                    BOB{}
                                        .append(kName, "g")
                                        .appendElements(TestingProctor::instance().isEnabled()
                                                            ? BOB{}.append(kPid, pid).obj()
                                                            : BOB{}.obj())
                                        .obj())
                            .append(kDriver, BOB{}.append(kName, "a").append(kVersion, "b").obj())
                            .append(kOperatingSystem,
                                    BOB{}
                                        .append(kType, "c")
                                        .append(kName, "d")
                                        .append(kArchitecture, "e")
                                        .append(kVersion, "f")
                                        .obj())
                            .obj())
                .obj();
        ASSERT_BSONOBJ_EQ(obj, outDoc);
    }

    // serializePrivate without appName
    {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serializePrivate(
            "a", "b", "c", "d", "e", "f", std::string{}, &builder));

        auto obj = builder.obj();

        BSONObj outDoc =
            BSON(kMetadataDoc << BSON(kDriver
                                      << BSON(kName << "a" << kVersion << "b") << kOperatingSystem
                                      << BSON(kType << "c" << kName << "d" << kArchitecture << "e"
                                                    << kVersion << "f")));
        ASSERT_BSONOBJ_EQ(obj, outDoc);
    }

    // Serialize with the os information automatically computed
    {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serialize("a", "b", "f", &builder));

        auto obj = builder.obj();

        auto swParse = ClientMetadata::parse(obj[kMetadataDoc]);
        ASSERT_OK(swParse.getStatus());
        ASSERT_EQUALS("f", swParse.getValue().value().getApplicationName());
    }
}

// Mixed: no client metadata document
TEST(ClientMetadataTest, TestEmptyDoc) {
    {
        auto parseStatus = ClientMetadata::parse(BSONElement());

        ASSERT_OK(parseStatus.getStatus());
    }

    {
        auto obj = BSON("client" << BSONObj());
        auto parseStatus = ClientMetadata::parse(obj[kMetadataDoc]);

        ASSERT_NOT_OK(parseStatus.getStatus());
    }
}

// Positive: test with only required fields
TEST(ClientMetadataTest, TestRequiredOnlyFields) {
    // Without app name
    ASSERT_DOC_OK(kDriver << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                          << BSON(kType << kUnknown));

    // With AppName
    ASSERT_DOC_OK(kApplication << BSON(kName << "1") << kDriver
                               << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                               << BSON(kType << kUnknown));
}


// Positive: test with app_name spelled wrong fields
TEST(ClientMetadataTest, TestWithAppNameSpelledWrong) {
    ASSERT_DOC_OK(kApplication << BSON("extra" << "1") << kDriver
                               << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                               << BSON(kType << kUnknown));
}

// Positive: test with empty application document
TEST(ClientMetadataTest, TestWithEmptyApplication) {
    ASSERT_DOC_OK(kApplication << BSONObj() << kDriver << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem << BSON(kType << kUnknown));
}

// Negative: test with appplication wrong type
TEST(ClientMetadataTest, TestNegativeWithAppNameWrongType) {
    ASSERT_DOC_NOT_OK(kApplication << "1" << kDriver << BSON(kName << "n1" << kVersion << "v1")
                                   << kOperatingSystem << BSON(kType << kUnknown));
}

// Positive: test with extra fields
TEST(ClientMetadataTest, TestExtraFields) {
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem << BSON(kType << kUnknown));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1"
                                             << "extra"
                                             << "v1")
                               << kOperatingSystem << BSON(kType << kUnknown));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown << "extra"
                                             << "v1"));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem << BSON(kType << kUnknown) << "extra"
                               << "v1");
}

// Negative: only application specified
TEST(ClientMetadataTest, TestNegativeOnlyApplication) {
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1"
                                                 << "extra"
                                                 << "v1"));
}

// Negative: all combinations of only missing 1 required field
TEST(ClientMetadataTest, TestNegativeMissingRequiredOneField) {
    ASSERT_DOC_NOT_OK(kDriver << BSON(kVersion << "v1") << kOperatingSystem
                              << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kDriver << BSON(kName << "n1") << kOperatingSystem
                              << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kDriver << BSON(kName << "n1" << kVersion << "v1"));
}

// Negative: document with wrong types for required fields
TEST(ClientMetadataTest, TestNegativeWrongTypes) {
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << 1) << kDriver
                                   << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << 1 << kVersion << "v1") << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << 1) << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                                   << BSON(kType << 1));
}

// Negative: document larger than 512 bytes
TEST(ClientMetadataTest, TestNegativeLargeDocument) {
    ScopeGuard unsetRouter([&] { serverGlobalParams.clusterRole = ClusterRole::None; });

    serverGlobalParams.clusterRole = ClusterRole::RouterServer;
    {
        std::string str(350, 'x');
        ASSERT_DOC_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << "1") << kOperatingSystem
                                   << BSON(kType << kUnknown) << "extra" << str);
    }
    {
        std::string str(512, 'x');
        ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                       << BSON(kName << "n1" << kVersion << "1") << kOperatingSystem
                                       << BSON(kType << kUnknown) << "extra" << str);
    }
}

// Negative: document with app_name larger than 128 bytes
TEST(ClientMetadataTest, TestNegativeLargeAppName) {
    {
        std::string str(128, 'x');
        ASSERT_DOC_OK(kApplication << BSON(kName << str) << kDriver
                                   << BSON(kName << "n1" << kVersion << "1") << kOperatingSystem
                                   << BSON(kType << kUnknown));

        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serialize("n1", "1", str, &builder));
    }
    {
        std::string str(129, 'x');
        ASSERT_DOC_NOT_OK(kApplication << BSON(kName << str) << kDriver
                                       << BSON(kName << "n1" << kVersion << "1") << kOperatingSystem
                                       << BSON(kType << kUnknown));

        BSONObjBuilder builder;
        ASSERT_NOT_OK(ClientMetadata::serialize("n1", "1", str, &builder));
    }
}

// Serialize and attach mongos information
TEST(ClientMetadataTest, TestMongoSAppend) {
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", "g", &builder));

    auto obj = builder.obj();
    auto swParseStatus = ClientMetadata::parse(obj[kMetadataDoc]);
    ASSERT_OK(swParseStatus.getStatus());
    auto metaObj = swParseStatus.getValue().value();
    ASSERT_EQUALS("g", metaObj.getApplicationName());
    auto docBeforeMongos = obj[kMetadataDoc].Obj();
    ASSERT_BSONOBJ_EQ(metaObj.getDocument(), docBeforeMongos);

    metaObj.setMongoSMetadata("h", "i", "j");
    ASSERT_BSONOBJ_NE(metaObj.getDocument(), docBeforeMongos);
    ASSERT_EQUALS("g", metaObj.getApplicationName());

    auto docWithMongosInfo = metaObj.getDocument();
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(), docBeforeMongos);

    auto pid = ProcessId::getCurrent().toString();

    using BOB = BSONObjBuilder;
    BSONObj outDoc =
        BOB{}
            .append(kApplication,
                    BOB{}
                        .append(kName, "g")
                        .appendElements(TestingProctor::instance().isEnabled()
                                            ? BOB{}.append(kPid, pid).obj()
                                            : BOB{}.obj())
                        .obj())
            .append(kDriver, BOB{}.append(kName, "a").append(kVersion, "b").obj())
            .append(kOperatingSystem,
                    BOB{}
                        .append(kType, "c")
                        .append(kName, "d")
                        .append(kArchitecture, "e")
                        .append(kVersion, "f")
                        .obj())
            .append(kMongos,
                    BOB{}.append(kHost, "h").append(kClient, "i").append(kVersion, "j").obj())
            .obj();
    ASSERT_BSONOBJ_EQ(docWithMongosInfo, outDoc);
}

// Test that if mongos information is present from the beginning, we can still request the document
// without the mongos info.
TEST(ClientMetadataTest, MongosMetaCanBeRemoved) {
    BSONObjBuilder realBuilder;
    BSONObjBuilder tmpBuilder;
    ASSERT_OK(ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", "g", &tmpBuilder));
    auto objWithoutMongosMeta = tmpBuilder.obj();
    const auto metaBsonNoMongosInfo = objWithoutMongosMeta[kMetadataDoc].Obj();
    {
        BSONObjBuilder metaBuilder = realBuilder.subobjStart(kMetadataDoc);
        metaBuilder.appendElements(metaBsonNoMongosInfo);
        metaBuilder.append("mongos", BSON(kHost << "h" << kClient << "i" << kVersion << "j"));
        metaBuilder.doneFast();
    }

    const auto wrappingMetaBson = realBuilder.obj();
    const auto metaElt = wrappingMetaBson[kMetadataDoc];
    // Add this mongos info without calling 'setMongoSMetadata().'
    ASSERT_BSONOBJ_NE(metaElt.Obj(), metaBsonNoMongosInfo);

    auto swParseStatus = ClientMetadata::parse(metaElt);
    ASSERT_OK(swParseStatus.getStatus());
    const auto& metaObj = swParseStatus.getValue().value();
    // Test the various copy/move constructors.
    ClientMetadata copyConstructed(metaObj);
    auto tmpThirdCopy = metaObj;
    ClientMetadata moveConstructed(std::move(tmpThirdCopy));

    auto tmpFourthCopy = metaObj;
    auto moveAssigned = metaObj;  // copy for now, until next line.
    moveAssigned = std::move(tmpFourthCopy);

    const auto tmpFifthCopy = metaObj;
    auto copyAssigned = metaObj;  // copy construct.
    copyAssigned = tmpFifthCopy;  // copy assign.

    ASSERT_BSONOBJ_EQ(metaObj.getDocument(), metaElt.Obj());
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(), metaBsonNoMongosInfo);
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(),
                      copyConstructed.documentWithoutMongosInfo());
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(),
                      moveConstructed.documentWithoutMongosInfo());
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(),
                      copyAssigned.documentWithoutMongosInfo());
    ASSERT_BSONOBJ_EQ(metaObj.documentWithoutMongosInfo(),
                      moveAssigned.documentWithoutMongosInfo());

    ASSERT_EQ(metaObj.hashWithoutMongosInfo(), copyConstructed.hashWithoutMongosInfo());
    ASSERT_EQ(metaObj.hashWithoutMongosInfo(), moveConstructed.hashWithoutMongosInfo());
    ASSERT_EQ(metaObj.hashWithoutMongosInfo(), copyAssigned.hashWithoutMongosInfo());
    ASSERT_EQ(metaObj.hashWithoutMongosInfo(), moveAssigned.hashWithoutMongosInfo());
}

TEST(ClientMetadataTest, TestInvalidDocWhileSettingOpCtxMetadata) {
    auto svcCtx = ServiceContext::make();
    auto client = svcCtx->getService()->makeClient("ClientMetadataTest");
    auto opCtx = client->makeOperationContext();
    auto obj = BSON("a" << 4);

    ASSERT_THROWS_CODE(ClientMetadata::setFromMetadataForOperation(opCtx.get(), obj),
                       DBException,
                       ErrorCodes::ClientMetadataMissingField);

    // Ensure we can still safely retrieve a ClientMetadata* from the opCtx, and that it was left
    // unset
    auto clientMetaDataPtr = ClientMetadata::getForOperation(opCtx.get());
    ASSERT_FALSE(clientMetaDataPtr);
}

TEST(ClientMetadataTest, InternalClientLimit) {
    auto svcCtx = ServiceContext::make();
    auto client = svcCtx->getService()->makeClient("ClientMetadataTest");

    std::string tooLargeValue(600, 'x');

    auto doc = BSON(kMetadataDoc << BSON(kDriver << BSON(kName << "n1" << kVersion << "v1")
                                                 << kOperatingSystem << BSON(kType << kUnknown)
                                                 << "extra" << tooLargeValue));
    auto el = doc.firstElement();

    // Succeeds because default limit is 1024 unless mongos (unit tests are not mongos)
    ASSERT_OK(ClientMetadata::parse(el).getStatus());

    // Throws since the document is too large
    ASSERT_THROWS_CODE(ClientMetadata::setFromMetadata(client.get(), el, false),
                       DBException,
                       ErrorCodes::ClientMetadataDocumentTooLarge);


    // Succeeds because internal client allows 1024
    ASSERT_DOES_NOT_THROW(ClientMetadata::setFromMetadata(client.get(), el, true));
}

// clientUpdate validation tests
TEST(ClientMetadataTest, TestClientUpdateValidDoc) {
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(BSON(kCid << "test-uuid")));
}

TEST(ClientMetadataTest, TestClientUpdateExtraFieldsAllowed) {
    auto doc = BSON(kCid << "test-uuid" << kDriver << BSON(kName << "n1" << kVersion << "v1")
                         << "config" << BSON("timeout" << 5000));
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(doc));
}

TEST(ClientMetadataTest, TestClientUpdateEmptyDoc) {
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(BSONObj()));
}

TEST(ClientMetadataTest, TestClientUpdateMissingCid) {
    auto doc = BSON(kDriver << BSON(kName << "n1" << kVersion << "v1"));
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(doc));
}

TEST(ClientMetadataTest, TestClientUpdateApplicationAllowed) {
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(
        BSON(kCid << "test-uuid" << kApplication << BSON(kName << "app1"))));
}

TEST(ClientMetadataTest, TestClientUpdateOsAllowed) {
    ASSERT_OK(ClientMetadata::validateClientMetadataUpdate(
        BSON(kCid << "test-uuid" << kOperatingSystem << BSON(kType << "Linux"))));
}

TEST(ClientMetadataTest, TestClientUpdateCidWrongType) {
    auto status = ClientMetadata::validateClientMetadataUpdate(BSON(kCid << 123));
    ASSERT_EQUALS(status.code(), ErrorCodes::TypeMismatch);
    ASSERT_STRING_CONTAINS(
        status.reason(), "The 'cid' field must be a string in the client metadata update document");
}


TEST(ClientMetadataTest, TestClientUpdateTooLarge) {
    std::string largeStr(1024, 'x');
    auto status = ClientMetadata::validateClientMetadataUpdate(BSON(kCid << largeStr));
    ASSERT_EQUALS(status.code(), ErrorCodes::ClientMetadataDocumentTooLarge);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "The client metadata update document must be less than or equal to");
}

class ClientMetadataUpdateLogRateLimiterFixture : public unittest::Test {
public:
    void setUp() override {
        _svcCtxClock = std::make_shared<ClockSourceMock>();
        _svcCtx = ServiceContext::make(std::make_unique<SharedClockSourceAdapter>(_svcCtxClock),
                                       std::make_unique<SharedClockSourceAdapter>(_svcCtxClock),
                                       std::make_unique<TickSourceMock<>>());
        _originalRatePerSec = gClientMetadataUpdateLogRatePerSec;
        _originalThrottleSecs = gClientMetadataUpdateLogPerConnectionThrottlingSecs.load();
        _originalNetworkSeverity =
            getGlobalLogSettings().getMinimumLogSeverity(logv2::LogComponent::kNetwork);
    }

    void tearDown() override {
        gClientMetadataUpdateLogRatePerSec = _originalRatePerSec;
        gClientMetadataUpdateLogPerConnectionThrottlingSecs.store(_originalThrottleSecs);
        getGlobalLogSettings().setMinimumLoggedSeverity(logv2::LogComponent::kNetwork,
                                                        _originalNetworkSeverity);
        ClientMetadata::setUpdateLogSuppressorClockSource_forTest(nullptr);
    }

protected:
    static void setGlobalClientUpdateLogRatePerSec(int32_t v) {
        gClientMetadataUpdateLogRatePerSec = v;
    }
    static void setPerConnectionClientUpdateLogThrottleSecs(int32_t v) {
        gClientMetadataUpdateLogPerConnectionThrottlingSecs.store(v);
    }
    static void setNetworkLogSeverity(logv2::LogSeverity s) {
        getGlobalLogSettings().setMinimumLoggedSeverity(logv2::LogComponent::kNetwork, s);
    }
    ClockSourceMock& getServiceContextClockMock() {
        return *_svcCtxClock;
    }

    ServiceContext::UniqueClient makeClient(std::string name = "TestClient") {
        return _svcCtx->getService()->makeClient(std::move(name), _transportMock.createSession());
    }
    static void logClientMetadataUpdate(Client* c) {
        static const BSONObj kDoc = BSON("driver" << BSON("name" << "T" << "version" << "1"));
        ClientMetadata::logClientMetadataUpdate(c, kDoc);
    }
    int64_t clientUpdateLogCount() {
        return _logs.countBSONContainingSubset(BSON("id" << 51817));
    }

private:
    static logv2::LogComponentSettings& getGlobalLogSettings() {
        return logv2::LogManager::global().getGlobalSettings();
    }

    std::shared_ptr<ClockSourceMock> _svcCtxClock;
    ServiceContext::UniqueServiceContext _svcCtx;
    transport::TransportLayerMock _transportMock;
    unittest::LogCaptureGuard _logs;

    int32_t _originalRatePerSec{};
    int32_t _originalThrottleSecs{};
    logv2::LogSeverity _originalNetworkSeverity{logv2::LogSeverity::Info()};
};

TEST_F(ClientMetadataUpdateLogRateLimiterFixture, GlobalRateLimiterZeroLogsAll) {
    setGlobalClientUpdateLogRatePerSec(0);
    setPerConnectionClientUpdateLogThrottleSecs(0);
    setNetworkLogSeverity(logv2::LogSeverity::Info());

    auto client = makeClient();
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());

    ASSERT_EQ(clientUpdateLogCount(), 3);
}

TEST_F(ClientMetadataUpdateLogRateLimiterFixture, GlobalRateLimiterSuppressesAtInfoVerbosity) {
    setGlobalClientUpdateLogRatePerSec(1);
    setPerConnectionClientUpdateLogThrottleSecs(0);
    setNetworkLogSeverity(logv2::LogSeverity::Info());

    ClockSourceMock suppressorClock;
    ClientMetadata::setUpdateLogSuppressorClockSource_forTest(&suppressorClock);

    auto client = makeClient();

    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 1);

    // suppressed to Debug(2), invisible
    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 1);

    // next period
    suppressorClock.advance(Milliseconds{1001});
    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 2);
}

TEST_F(ClientMetadataUpdateLogRateLimiterFixture, GlobalRateLimiterSuppressedVisibleAtDebug) {
    setGlobalClientUpdateLogRatePerSec(1);
    setPerConnectionClientUpdateLogThrottleSecs(0);
    setNetworkLogSeverity(logv2::LogSeverity::Debug(5));

    ClockSourceMock suppressorClock;
    ClientMetadata::setUpdateLogSuppressorClockSource_forTest(&suppressorClock);

    auto client = makeClient();
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());

    ASSERT_EQ(clientUpdateLogCount(), 3);
}

TEST_F(ClientMetadataUpdateLogRateLimiterFixture, PerConnectionThrottleSkipsWithinInterval) {
    setGlobalClientUpdateLogRatePerSec(0);
    setPerConnectionClientUpdateLogThrottleSecs(1800);

    // First per-connection check compares against epoch 0; skip past it.
    getServiceContextClockMock().advance(Seconds{2000});
    auto client = makeClient();

    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 1);

    // interval not elapsed
    getServiceContextClockMock().advance(Seconds{100});
    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 1);

    // interval elapsed
    getServiceContextClockMock().advance(Seconds{1701});
    logClientMetadataUpdate(client.get());
    ASSERT_EQ(clientUpdateLogCount(), 2);
}

TEST_F(ClientMetadataUpdateLogRateLimiterFixture, PerConnectionThrottleZeroLogsEveryUpdate) {
    setGlobalClientUpdateLogRatePerSec(0);
    setPerConnectionClientUpdateLogThrottleSecs(0);

    auto client = makeClient();
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());
    logClientMetadataUpdate(client.get());

    ASSERT_EQ(clientUpdateLogCount(), 3);
}

}  // namespace mongo
