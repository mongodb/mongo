/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/address_restriction.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo {
namespace {


TEST(AddressRestrictionTest, toAndFromString) {
    const struct {
        std::string input;
        std::string output;
    } strings[] = {
        {"127.0.0.1/8", "127.0.0.1/8"},
        {"127.0.0.1", "127.0.0.1/32"},
        {"::1", "::1/128"},
        {"169.254.0.0/16", "169.254.0.0/16"},
        {"fe80::/10", "fe80::/10"},
    };
    for (auto&& p : strings) {
        {
            const ClientSourceRestriction csr(CIDR(p.input));
            std::ostringstream actual;
            actual << csr;
            std::ostringstream expected;
            expected << "{\"clientSource\": \"" << p.output << "\"}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }

        {
            const ServerAddressRestriction sar(CIDR(p.input));
            std::ostringstream actual;
            actual << sar;
            std::ostringstream expected;
            expected << "{\"serverAddress\": \"" << p.output << "\"}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }
    }
}

TEST(AddressRestrictionTest, contains) {
    enableIPv6(true);
    const struct {
        std::string range;
        std::string address;
        bool valid;
    } contains[] = {
        {"127.0.0.1/8", "0.0.0.0", false},
        {"127.0.0.1/8", "0.0.0.1", false},
        {"127.0.0.1/8", "126.255.255.255", false},
        {"127.0.0.1/8", "127.0.0.0", true},
        {"127.0.0.1/8", "127.0.0.1", true},
        {"127.0.0.1/8", "127.0.0.127", true},
        {"127.0.0.1/8", "127.0.0.128", true},
        {"127.0.0.1/8", "127.0.0.255", true},
        {"127.0.0.1/8", "127.255.255.255", true},
        {"127.0.0.1/8", "127.127.128.1", true},
        {"127.0.0.1/8", "127.128.127.0", true},
        {"127.0.0.1/8", "128.0.0.1", false},
        {"127.0.0.1/8", "169.254.13.37", false},
        {"127.0.0.1/8", "255.0.0.1", false},
        {"127.0.0.1/8", "255.255.255.254", false},
        {"127.0.0.1/8", "255.255.255.255", false},

        {"127.0.0.1", "126.255.255.255", false},
        {"127.0.0.1", "127.0.0.0", false},
        {"127.0.0.1", "126.0.0.1", false},
        {"127.0.0.1", "127.0.0.1", true},
        {"127.0.0.1", "127.0.0.2", false},
        {"127.0.0.1", "127.0.0.127", false},
        {"127.0.0.1", "127.0.0.128", false},
        {"127.0.0.1", "127.0.0.255", false},
        {"127.0.0.1", "127.1.0.1", false},
        {"127.0.0.1", "127.128.0.1", false},
        {"127.0.0.1", "128.0.0.1", false},

        {"127.0.0.2/31", "127.0.0.0", false},
        {"127.0.0.2/31", "127.0.0.1", false},
        {"127.0.0.2/31", "127.0.0.2", true},
        {"127.0.0.2/31", "127.0.0.3", true},
        {"127.0.0.2/31", "127.0.0.4", false},
        {"127.0.0.2/31", "127.0.1.1", false},

        {"169.254.80.0/20", "169.254.79.255", false},
        {"169.254.80.0/20", "169.254.80.0", true},
        {"169.254.80.0/20", "169.254.95.255", true},
        {"169.254.80.0/20", "169.254.96.0", false},

        {"169.254.0.160/28", "169.254.0.159", false},
        {"169.254.0.160/28", "169.254.0.160", true},
        {"169.254.0.160/28", "169.254.0.175", true},
        {"169.254.0.160/28", "169.254.0.176", false},

        {"169.254.0.0/16", "8.8.8.8", false},
        {"169.254.0.0/16", "127.0.0.1", false},
        {"169.254.0.0/16", "169.254.0.0", true},
        {"169.254.0.0/16", "169.254.0.1", true},
        {"169.254.0.0/16", "169.254.31.17", true},
        {"169.254.0.0/16", "169.254.255.254", true},
        {"169.254.0.0/16", "169.254.255.255", true},
        {"169.254.0.0/16", "240.0.0.1", false},
        {"169.254.0.0/16", "255.255.255.255", false},

        {"::1", "::", false},
        {"::1", "::0", false},
        {"::1", "::1", true},
        {"::1", "::2", false},
        {"::1", "::1:0", false},
        {"::1", "::1:0:0", false},
        {"::1", "1::", false},
        {"::1", "1::1", false},
        {"::1", "8000::1", false},

        {"::1/96", "::1", true},
        {"::1/96", "::1:0:0", false},
        {"::1/96", "::DEAD:BEEF", true},
        {"::1/96", "DEAD::BEEF", false},

        {"::2/127", "::0", false},
        {"::2/127", "::1", false},
        {"::2/127", "::2", true},
        {"::2/127", "::3", true},
        {"::2/127", "::4", false},
        {"::2/127", "::5", false},

        {"::1/128", "::", false},
        {"::1/128", "::0", false},
        {"::1/128", "::1", true},
        {"::1/128", "::2", false},
        {"::1/128", "8000::", false},
    };
    for (const auto& p : contains) {
        const SockAddr dummy;
        const SockAddr addr(p.address, 1024);
        const RestrictionEnvironment rec(addr, dummy);
        const RestrictionEnvironment res(dummy, addr);

        const ClientSourceRestriction csr(CIDR(p.range));
        ASSERT_EQ(csr.validate(rec).isOK(), p.valid);
        ASSERT_FALSE(csr.validate(res).isOK());

        const ServerAddressRestriction sar(CIDR(p.range));
        ASSERT_EQ(sar.validate(res).isOK(), p.valid);
        ASSERT_FALSE(sar.validate(rec).isOK());
    }
}

}  // namespace
}  // namespace mongo
