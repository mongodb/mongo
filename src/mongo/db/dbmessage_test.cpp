// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/dbmessage.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
using std::string;

// Test if the reserved field is short of 4 bytes
TEST(DBMessage1, ShortFlags) {
    BufBuilder b;
    string ns("test");

    b.appendChar(1);

    Message toSend;
    toSend.setData(dbDelete, b.buf(), b.len());

    ASSERT_THROWS(DbMessage(toSend), AssertionException);
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

    ASSERT_THROWS(DbMessage(toSend), AssertionException);
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

    ASSERT_THROWS(d1.pullInt(), AssertionException);
}

// Try a bad read of a type too large
TEST(DBMessage1, GoodKill2) {
    BufBuilder b;

    b.appendNum(static_cast<int>(1));
    b.appendNum(static_cast<int>(3));

    Message toSend;
    toSend.setData(dbKillCursors, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_THROWS(d1.pullInt64(), AssertionException);
}

// Test a basic good insert, and an extra read
TEST(DBMessage1, GoodInsert) {
    BufBuilder b;
    string ns("test");

    b.appendNum(static_cast<int>(1));
    b.appendCStr(ns);
    b.appendNum(static_cast<int>(3));
    b.appendNum(static_cast<int>(39));

    Message toSend;
    toSend.setData(dbInsert, b.buf(), b.len());

    DbMessage d1(toSend);
    ASSERT_EQUALS(3, d1.pullInt());
    ASSERT_EQUALS(39, d1.pullInt());
    ASSERT_THROWS(d1.pullInt(), AssertionException);
}

// Test a basic good insert, and an extra read
TEST(DBMessage1, GoodInsert2) {
    BufBuilder b;
    string ns("test");

    b.appendNum(static_cast<int>(1));
    b.appendCStr(ns);
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
    ASSERT_THROWS(d1.nextJsObj(), AssertionException);
}


}  // namespace mongo
