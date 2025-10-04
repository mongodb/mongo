/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        const auto service = getServiceContext();
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings{});
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
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
