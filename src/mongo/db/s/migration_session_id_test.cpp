/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_session_id.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

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
    MigrationSessionId emptySessionId =
        assertGet(MigrationSessionId::extractFromBSON(BSON("SomeField" << 1)));
    MigrationSessionId nonEmptySessionId =
        assertGet(MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId"
                                                                       << "TestSessionID")));

    ASSERT(!emptySessionId.matches(nonEmptySessionId));
    ASSERT(!nonEmptySessionId.matches(emptySessionId));

    MigrationSessionId sessionIdToCompare =
        assertGet(MigrationSessionId::extractFromBSON(BSON("SomeOtherField" << 1 << "sessionId"
                                                                            << "TestSessionID")));
    ASSERT(nonEmptySessionId.matches(sessionIdToCompare));
    ASSERT(sessionIdToCompare.matches(nonEmptySessionId));
}

TEST(MigrationSessionId, ErrorWhenTypeIsNotString) {
    ASSERT_NOT_OK(
        MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId" << Date_t::now()))
            .getStatus());
    ASSERT_NOT_OK(MigrationSessionId::extractFromBSON(BSON("SomeField" << 1 << "sessionId" << 2))
                      .getStatus());
}

}  // namespace
}  // namespace mongo
