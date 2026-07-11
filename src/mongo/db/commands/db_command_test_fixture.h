// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
class DBCommandTestFixture : public ServiceContextMongoDTest {
public:
    using Options = ServiceContextMongoDTest::Options;
    using ServiceContextMongoDTest::ServiceContextMongoDTest;

    void setUp() override {
        ServiceContextMongoDTest::setUp();

        const auto service = getServiceContext();
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings{});
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        replCoord->setCanAcceptNonLocalWrites(true);
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(opCtx);
    }

    BSONObj runCommand(BSONObj cmd,
                       const DatabaseName& dbName = kDatabaseName,
                       boost::optional<long> errorCode = boost::none) {
        DBDirectClient client(opCtx);

        BSONObj result;
        if (!errorCode) {
            ASSERT_TRUE(client.runCommand(dbName, cmd, result)) << result;
        } else {
            ASSERT_FALSE(client.runCommand(dbName, cmd, result)) << result;
            ASSERT_TRUE(result.hasField("code") && result["code"].isNumber()) << result;
            ASSERT_EQ(result["code"].Number(), *errorCode) << result;
        }
        return result;
    }

    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* opCtx{_uniqueOpCtx.get()};

    static const DatabaseName kDatabaseName;
};

inline const DatabaseName DBCommandTestFixture::kDatabaseName =
    DatabaseName::createDatabaseName_forTest(boost::none, "unittest_db");

}  // namespace mongo
