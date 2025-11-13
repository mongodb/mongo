/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/remote_command_targeter.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <array>

namespace mongo {

class RemoteCommandTargeterTest : public unittest::Test {
protected:
    static inline std::array hosts = {
        HostAndPort{"fakehost1", 12345},
        HostAndPort{"fakehost2", 12345},
        HostAndPort{"fakehost3", 12345},
        HostAndPort{"fakehost4", 12345},
    };

    static_assert(hosts.size() > 2);

    static inline std::array primaryOnly = {
        HostAndPort{"fakehost1", 12345},
    };

    static inline std::array<HostAndPort, 0> emptyDeprioritized = {};
    static inline std::vector<HostAndPort> everyButFirstDeprioritized{hosts.begin() + 1,
                                                                      hosts.end()};
    static inline std::vector<HostAndPort> allHostsDeprioritized{hosts.begin(), hosts.end()};
    static inline std::array<HostAndPort, 1> firstHostsDeprioritized{hosts.front()};
    static inline std::array firstAndSecondHostsDeprioritized{hosts[0], hosts[1]};
};

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedEmpty) {
    const auto& result = RemoteCommandTargeter::firstHostPrioritized(hosts, emptyDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedFirstHostDepriotitized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(hosts, firstHostsDeprioritized);
    ASSERT_EQ(result, hosts[1]);
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedFirstAndSecondHostDepriotitized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(hosts, firstAndSecondHostsDeprioritized);
    ASSERT_EQ(result, hosts[2]);
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedEveryButFirstDeprioritized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(hosts, everyButFirstDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedAllHostsDeprioritized) {
    const auto& result = RemoteCommandTargeter::firstHostPrioritized(hosts, allHostsDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedAllHostsPrimaryOnlyBasic) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(primaryOnly, emptyDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedAllHostsPrimaryOnlyFirstHostDeprioritized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(primaryOnly, firstHostsDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest,
       FirstHostPrioritizedAllHostsPrimaryOnlyFirstAndSecondHostDeprioritized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(primaryOnly, firstAndSecondHostsDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedPrimaryOnlyEveryButFirstDeprioritized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(primaryOnly, everyButFirstDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

TEST_F(RemoteCommandTargeterTest, FirstHostPrioritizedPrimaryOnlyAllHostsDeprioritized) {
    const auto& result =
        RemoteCommandTargeter::firstHostPrioritized(primaryOnly, allHostsDeprioritized);
    ASSERT_EQ(result, hosts.front());
}

}  // namespace mongo
