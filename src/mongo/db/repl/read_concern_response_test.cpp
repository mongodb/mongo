/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/read_concern_response.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

TEST(ReadAfterResponse, Default) {
    ReadConcernResponse response;

    ASSERT_FALSE(response.didWait());

    BSONObjBuilder builder;
    response.appendInfo(&builder);

    BSONObj obj(builder.done());
    ASSERT_TRUE(obj.isEmpty());
}

TEST(ReadAfterResponse, WithStatus) {
    ReadConcernResponse response(Status(ErrorCodes::InternalError, "test"));

    ASSERT_FALSE(response.didWait());

    ASSERT_EQ(ErrorCodes::InternalError, response.getStatus().code());

    BSONObjBuilder builder;
    response.appendInfo(&builder);

    BSONObj obj(builder.done());
    ASSERT_TRUE(obj.isEmpty());
}

TEST(ReadAfterResponse, WaitedWithDuration) {
    ReadConcernResponse response(Status(ErrorCodes::InternalError, "test"), Milliseconds(7));

    ASSERT_TRUE(response.didWait());
    ASSERT_EQUALS(Milliseconds(7), response.getDuration());
    ASSERT_EQ(ErrorCodes::InternalError, response.getStatus().code());

    BSONObjBuilder builder;
    response.appendInfo(&builder);

    BSONObj obj(builder.done());
    auto waitedMSElem = obj[ReadConcernResponse::kWaitedMSFieldName];
    ASSERT_TRUE(waitedMSElem.isNumber());
    ASSERT_EQ(7, waitedMSElem.numberLong());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo
