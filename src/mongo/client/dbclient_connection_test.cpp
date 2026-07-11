// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
