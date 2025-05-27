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

#include "mongo/client/dbclient_connection.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

class DBClientConnectionFixture : public unittest::Test {};

TEST_F(DBClientConnectionFixture, TestTimeOutCtor) {
    ASSERT_EQUALS(DBClientConnection(true, 10).getSoTimeout(), 10);
    ASSERT_EQUALS(DBClientConnection(true, 0).getSoTimeout(), 0);
    ASSERT_EQUALS(DBClientConnection(true, -1).getSoTimeout(), 0);

    auto timeoutMx = Milliseconds::max().count() / 1000;
    ASSERT_EQUALS(DBClientConnection(true, timeoutMx).getSoTimeout(), timeoutMx);

    auto timeout = timeoutMx + 1.0;
    ASSERT_EQUALS(DBClientConnection(true, timeout).getSoTimeout(), timeoutMx);
}

TEST_F(DBClientConnectionFixture, TestTimeOutSetter) {
    auto session = DBClientConnection(true);

    session.setSoTimeout(10);
    ASSERT_EQUALS(session.getSoTimeout(), 10);

    session.setSoTimeout(-1);
    ASSERT_EQUALS(session.getSoTimeout(), 0);

    auto timeoutMx = Milliseconds::max().count() / 1000;
    session.setSoTimeout(timeoutMx);
    ASSERT_EQUALS(session.getSoTimeout(), timeoutMx);

    auto timeout = timeoutMx + 1.0;
    session.setSoTimeout(timeout);
    ASSERT_EQUALS(session.getSoTimeout(), timeoutMx);
}

}  // namespace mongo
