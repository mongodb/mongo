// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {

using Mock = ChangeStreamReaderBuilderMock;

using BuildShardTargeterFunction = Mock::BuildShardTargeterFunction;
using BuildControlEventFunction = Mock::BuildControlEventFunction;
using GetControlEventTypesFunction = Mock::GetControlEventTypesFunction;

class ChangeStreamReaderBuilderTest : public ServiceContextTest {
public:
    ChangeStreamReaderBuilderTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {
        _opCtx = makeOperationContext();
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

    std::unique_ptr<ChangeStreamReaderBuilderMock> mock;

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ChangeStreamReaderBuilderTest, DefaultMockReturnsEmptyValues) {
    ChangeStream changeStream(
        ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, {} /* nss */);

    mock = std::make_unique<ChangeStreamReaderBuilderMock>();
    ASSERT_EQ(nullptr, mock->buildShardTargeter(getOpCtx(), changeStream));
    ASSERT_BSONOBJ_EQ(BSONObj(),
                      mock->buildControlEventFilterForDataShard(getOpCtx(), changeStream));
    ASSERT_EQ(std::set<std::string>{},
              mock->getControlEventTypesOnDataShard(getOpCtx(), changeStream));
    ASSERT_BSONOBJ_EQ(BSONObj(),
                      mock->buildControlEventFilterForConfigServer(getOpCtx(), changeStream));
    ASSERT_EQ(std::set<std::string>{},
              mock->getControlEventTypesOnConfigServer(getOpCtx(), changeStream));
}

TEST_F(ChangeStreamReaderBuilderTest, OperationContextAndChangeStreamAreCorrectlyPassedThrough) {
    ChangeStream changeStream(
        ChangeStreamReadMode::kStrict,
        ChangeStreamType::kCollection,
        NamespaceString::createNamespaceString_forTest("testDB.testCollection"));

    // Validate that we get the expected objects passed.
    auto validate = [&](OperationContext* opCtx, const ChangeStream& cs) {
        ASSERT_EQ(getOpCtx(), opCtx);
        ASSERT_EQ(&changeStream, &cs);
    };

    // Validates the opCtx and change stream pointers upon every invocation of the mock.
    mock = std::make_unique<ChangeStreamReaderBuilderMock>(
        [&](OperationContext* opCtx, const ChangeStream& changeStream) {
            validate(opCtx, changeStream);
            return [](OperationContext*, const ChangeStream&) {
                return std::unique_ptr<ChangeStreamShardTargeter>();
            }(opCtx, changeStream);
        },
        [&](OperationContext* opCtx, const ChangeStream& changeStream) {
            validate(opCtx, changeStream);
            return [](OperationContext*, const ChangeStream&) {
                return BSONObj();
            }(opCtx, changeStream);
        },
        [&](OperationContext* opCtx, const ChangeStream& changeStream) {
            validate(opCtx, changeStream);
            return [](OperationContext*, const ChangeStream&) {
                return std::set<std::string>{};
            }(opCtx, changeStream);
        },
        [&](OperationContext* opCtx, const ChangeStream& changeStream) {
            validate(opCtx, changeStream);
            return [](OperationContext*, const ChangeStream&) {
                return BSONObj();
            }(opCtx, changeStream);
        },
        [&](OperationContext* opCtx, const ChangeStream& changeStream) {
            validate(opCtx, changeStream);
            return [](OperationContext*, const ChangeStream&) {
                return std::set<std::string>{};
            }(opCtx, changeStream);
        });

    ASSERT_EQ(nullptr, mock->buildShardTargeter(getOpCtx(), changeStream));
    ASSERT_BSONOBJ_EQ(BSONObj(),
                      mock->buildControlEventFilterForDataShard(getOpCtx(), changeStream));
    ASSERT_EQ(std::set<std::string>{},
              mock->getControlEventTypesOnDataShard(getOpCtx(), changeStream));
    ASSERT_BSONOBJ_EQ(BSONObj(),
                      mock->buildControlEventFilterForConfigServer(getOpCtx(), changeStream));
    ASSERT_EQ(std::set<std::string>{},
              mock->getControlEventTypesOnConfigServer(getOpCtx(), changeStream));
}

TEST_F(ChangeStreamReaderBuilderTest, DecorationsOnServiceContextWork) {
    ChangeStream changeStream(
        ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, {} /* nss */);

    // Note that these are just made up filters for testing and do not resemble the true filters on
    // the oplog.
    BSONObj controlEventFilterForDataShard =
        BSON("op" << "n" << "o2.someEvent" << BSON("$exists" << true));
    std::set<std::string> controlEventTypesOnDataShard = {"movePrimary", "moveChunk"};
    BSONObj controlEventFilterForConfigServer = BSON("op" << "i" << "ns.db" << "config");
    std::set<std::string> controlEventTypesOnConfigServer = {"insert"};

    auto decoratedMock = std::make_unique<ChangeStreamReaderBuilderMock>(
        [](OperationContext*, const ChangeStream&) {
            return std::unique_ptr<ChangeStreamShardTargeter>();
        },
        [&](OperationContext*, const ChangeStream&) { return controlEventFilterForDataShard; },
        [&](OperationContext*, const ChangeStream&) { return controlEventTypesOnDataShard; },
        [&](OperationContext*, const ChangeStream&) { return controlEventFilterForConfigServer; },
        [&](OperationContext*, const ChangeStream&) { return controlEventTypesOnConfigServer; });

    // Register the mock with the service context.
    ChangeStreamReaderBuilder::set(getServiceContext(), std::move(decoratedMock));

    // Call the decorations on the ServiceContext.
    ASSERT_EQ(controlEventTypesOnDataShard,
              ChangeStreamReaderBuilder::get(getServiceContext())
                  ->getControlEventTypesOnDataShard(getOpCtx(), changeStream));

    ASSERT_BSONOBJ_EQ(controlEventFilterForDataShard,
                      ChangeStreamReaderBuilder::get(getServiceContext())
                          ->buildControlEventFilterForDataShard(getOpCtx(), changeStream));

    ASSERT_EQ(controlEventTypesOnConfigServer,
              ChangeStreamReaderBuilder::get(getServiceContext())
                  ->getControlEventTypesOnConfigServer(getOpCtx(), changeStream));

    ASSERT_BSONOBJ_EQ(controlEventFilterForConfigServer,
                      ChangeStreamReaderBuilder::get(getServiceContext())
                          ->buildControlEventFilterForConfigServer(getOpCtx(), changeStream));
}

}  // namespace mongo
