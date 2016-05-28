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

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

TEST(ChangeLogType, Empty) {
    auto changeLogResult = ChangeLogType::fromBSON(BSONObj());
    ASSERT_NOT_OK(changeLogResult.getStatus());
}

TEST(ChangeLogType, Valid) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::server("host.local")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_OK(changeLogResult.getStatus());
    ChangeLogType& logEntry = changeLogResult.getValue();
    ASSERT_OK(logEntry.validate());

    ASSERT_EQUALS(logEntry.getChangeId(), "host.local-2012-11-21T19:14:10-8");
    ASSERT_EQUALS(logEntry.getServer(), "host.local");
    ASSERT_EQUALS(logEntry.getClientAddr(), "192.168.0.189:51128");
    ASSERT_EQUALS(logEntry.getTime(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(logEntry.getWhat(), "split");
    ASSERT_EQUALS(logEntry.getNS(), "test.test");
    ASSERT_EQUALS(logEntry.getDetails(),
                  BSON("dummy"
                       << "info"));
}

TEST(ChangeLogType, MissingChangeId) {
    BSONObj obj = BSON(ChangeLogType::server("host.local")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, MissingServer) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, MissingClientAddr) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::server("host.local")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, MissingTime) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::server("host.local")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, MissingWhat) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::server("host.local")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::ns("test.test")
                       << ChangeLogType::details(BSON("dummy"
                                                      << "info")));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, MissingDetails) {
    BSONObj obj = BSON(ChangeLogType::changeId("host.local-2012-11-21T19:14:10-8")
                       << ChangeLogType::server("host.local")
                       << ChangeLogType::clientAddr("192.168.0.189:51128")
                       << ChangeLogType::time(Date_t::fromMillisSinceEpoch(1))
                       << ChangeLogType::what("split")
                       << ChangeLogType::ns("test.test"));

    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
}

TEST(ChangeLogType, BadType) {
    ChangeLogType logEntry;
    BSONObj obj = BSON(ChangeLogType::changeId() << 0);
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, changeLogResult.getStatus());
}

}  // unnamed namespace
