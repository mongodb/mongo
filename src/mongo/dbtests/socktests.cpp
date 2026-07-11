// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * sock.{h,cpp} unit tests.
 */

#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"

#include <string>
#include <vector>

namespace mongo {
namespace SockTests {

class HostByName {
public:
    void run() {
        ASSERT_EQUALS("127.0.0.1", hostbyname("localhost"));
        ASSERT_EQUALS("127.0.0.1", hostbyname("127.0.0.1"));
        // ASSERT_EQUALS( "::1", hostbyname( "::1" ) ); // IPv6 disabled at runtime by default.

        HostAndPort h("asdfasdfasdf_no_such_host");
        ASSERT_EQUALS("", hostbyname("asdfasdfasdf_no_such_host"));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("sock") {}
    void setupTests() override {
        add<HostByName>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace SockTests
}  // namespace mongo
