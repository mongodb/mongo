/**
 * Copyright (C) 2014 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include <string>

#include "mongo/bson/util/builder.h"
#include "mongo/db/dbmessage.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using std::string;

// Test if the reserved field is short of 4 bytes
TEST(DBMessage1, ShortFlags) {
    BufBuilder b;
    string ns("test");

    b.appendChar(1);

    Message toSend;
    toSend.setData(dbDelete, b.buf(), b.len());

    ASSERT_THROWS(DbMessage d1(toSend), UserException);
}

// Test a short NS missing a trailing null
TEST(DBMessage1, BadNS) {
    BufBuilder b;

    b.appendNum(static_cast<int>(1));
    b.appendChar('b');
    b.appendChar('a');
    b.appendChar('d');
    // Forget to append \0

    Message toSend;
    toSend.setData(dbDelete, b.buf(), b.len());

    ASSERT_THROWS(DbMessage d1(toSend), UserException);
}

// Test a valid kill message and try an extra pull
TEST(DBMessage1, GoodKill) {
    BufBuilder b;

    b.appendNum(static_cast<int>(1));
    b.appendNum(static_cast<int>(3));

    Message toSend;
    toSend.setData(dbKillCursors, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_EQUALS(3, d1.pullInt());

    ASSERT_THROWS(d1.pullInt(), UserException);
}

// Try a bad read of a type too large
TEST(DBMessage1, GoodKill2) {
    BufBuilder b;

    b.appendNum(static_cast<int>(1));
    b.appendNum(static_cast<int>(3));

    Message toSend;
    toSend.setData(dbKillCursors, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_THROWS(d1.pullInt64(), UserException);
}

// Test a basic good insert, and an extra read
TEST(DBMessage1, GoodInsert) {
    BufBuilder b;
    string ns("test");

    b.appendNum(static_cast<int>(1));
    b.appendStr(ns);
    b.appendNum(static_cast<int>(3));
    b.appendNum(static_cast<int>(39));

    Message toSend;
    toSend.setData(dbInsert, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_EQUALS(3, d1.pullInt());
    ASSERT_EQUALS(39, d1.pullInt());
    ASSERT_THROWS(d1.pullInt(), UserException);
}

// Test a basic good insert, and an extra read
TEST(DBMessage1, GoodInsert2) {
    BufBuilder b;
    string ns("test");

    b.appendNum(static_cast<int>(1));
    b.appendStr(ns);
    b.appendNum(static_cast<int>(3));
    b.appendNum(static_cast<int>(39));

    BSONObj bo = BSON("ts" << 0);
    bo.appendSelfToBufBuilder(b);

    Message toSend;
    toSend.setData(dbInsert, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_EQUALS(3, d1.pullInt());


    ASSERT_EQUALS(39, d1.pullInt());
    BSONObj bo2 = d1.nextJsObj();
    ASSERT_THROWS(d1.nextJsObj(), MsgAssertionException);
}


}  // mongo namespace
