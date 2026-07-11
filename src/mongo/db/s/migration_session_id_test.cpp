// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/migration_session_id.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <memory>

#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

TEST(MigrationSessionId, GenerateAndExtract) {
    MigrationSessionId origSessionId = MigrationSessionId::generate("Source", "Dest");

    BSONObjBuilder builder;
    origSessionId.append(&builder);
    BSONObj obj = builder.obj();

    MigrationSessionId sessionIdAfter = assertGet(MigrationSessionId::extractFromBSON(obj));
    ASSERT(origSessionId.matches(sessionIdAfter));
    ASSERT_EQ(origSessionId.toString(), sessionIdAfter.toString());
}

TEST(MigrationSessionId, Comparison) {
    MigrationSessionId sessionId =
        assertGet(MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId"
                                                                       << "TestSessionID")));

    MigrationSessionId sessionIdToCompare =
        assertGet(MigrationSessionId::extractFromBSON(BSON("SomeOtherField" << 1 << "sessionId"
                                                                            << "TestSessionID")));
    ASSERT(sessionId.matches(sessionIdToCompare));
    ASSERT(sessionIdToCompare.matches(sessionId));
}

TEST(MigrationSessionId, ErrorNoSuchKeyWhenSessionIdIsMissing) {
    ASSERT_EQ(ErrorCodes::NoSuchKey,
              MigrationSessionId::extractFromBSON(BSON("SomeField" << 1)).getStatus().code());
}

TEST(MigrationSessionId, ErrorWhenSessionIdTypeIsNotString) {
    ASSERT_NOT_OK(
        MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId" << Date_t::now()))
            .getStatus());
    ASSERT_NOT_OK(MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId" << 2))
                      .getStatus());
}

}  // namespace
}  // namespace mongo
