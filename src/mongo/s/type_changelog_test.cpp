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

#include "mongo/s/type_changelog.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

    using std::string;
    using mongo::ChangelogType;
    using mongo::BSONObj;
    using mongo::Date_t;

    TEST(Validity, Empty) {
        ChangelogType logEntry;
        BSONObj emptyObj = BSONObj();
        string errMsg;
        ASSERT(logEntry.parseBSON(emptyObj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, Valid) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(logEntry.isValid(NULL));
        ASSERT_EQUALS(logEntry.getChangeID(), "host.local-2012-11-21T19:14:10-8");
        ASSERT_EQUALS(logEntry.getServer(), "host.local");
        ASSERT_EQUALS(logEntry.getClientAddr(), "192.168.0.189:51128");
        ASSERT_EQUALS(logEntry.getTime(), 1ULL);
        ASSERT_EQUALS(logEntry.getWhat(), "split");
        ASSERT_EQUALS(logEntry.getNS(), "test.test");
        ASSERT_EQUALS(logEntry.getDetails(), BSON("dummy" << "info"));
    }

    TEST(Validity, MissingChangeID) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingServer) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingClientAddr) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingTime) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingWhat) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::ns("test.test") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingNS) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::details(BSON("dummy" << "info")));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, MissingDetails) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8") <<
                           ChangelogType::server("host.local") <<
                           ChangelogType::clientAddr("192.168.0.189:51128") <<
                           ChangelogType::time(1ULL) <<
                           ChangelogType::what("split") <<
                           ChangelogType::ns("test.test"));
        string errMsg;
        ASSERT(logEntry.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(logEntry.isValid(NULL));
    }

    TEST(Validity, BadType) {
        ChangelogType logEntry;
        BSONObj obj = BSON(ChangelogType::changeID() << 0);
        string errMsg;
        ASSERT((!logEntry.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
