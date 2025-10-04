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
