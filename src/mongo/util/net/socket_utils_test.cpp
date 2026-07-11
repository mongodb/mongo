// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/socket_utils.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(UnixSockPath, mustContainHyphen) {
    const auto path = "/tmp/mongodb27018.sock";
    ASSERT_EQ(parsePortFromUnixSockPath(path), -1);
}

TEST(UnixSockPath, mustContainPort) {
    const auto path = "/tmp/mongodb-.sock";
    ASSERT_EQ(parsePortFromUnixSockPath(path), -1);
}

TEST(UnixSockPath, mustHaveCorrectExtension) {
    const auto path = "/tmp/mongodb-27018";
    ASSERT_EQ(parsePortFromUnixSockPath(path), -1);
}

TEST(UnixSockPath, inverseOfMakeUnixSockPath) {
    const int port = 27018;
    const auto path = makeUnixSockPath(port);
    ASSERT_EQ(parsePortFromUnixSockPath(path), port);
}

}  // namespace mongo

