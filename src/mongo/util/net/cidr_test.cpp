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

#include "mongo/unittest/unittest.h"
#include "mongo/util/net/cidr.h"

namespace mongo {
namespace {

using string_pair = std::pair<std::string, std::string>;

TEST(CIDRTest, toAndFromString) {
    string_pair strings[] = {
        {"127.0.0.1/8", "127.0.0.1/8"},
        {"127.0.0.1", "127.0.0.1/32"},
        {"::1", "::1/128"},
        {"169.254.0.0/16", "169.254.0.0/16"},
        {"fe80::/10", "fe80::/10"},
    };
    for (auto&& p : strings) {
        CIDR x(p.first);
        ASSERT_EQUALS(x.toString(), p.second);

        std::ostringstream s;
        s << x;
        ASSERT_EQUALS(s.str(), p.second);

        StringBuilder sb;
        sb << x;
        ASSERT_EQUALS(sb.str(), p.second);
    }
}

TEST(CIDRTest, contains) {
    struct {
        std::string range;
        std::string value;
        bool should_match;
    } test_data[] = {
        {"127.0.0.1/8", "127.0.0.1", true},
        {"127.0.0.1/8", "127.0.0.3", true},
        {"127.0.0.1/8", "127.18.32.41", true},
        {"127.0.0.0/8", "127.128.0.0", true},
        {"127.0.0.1", "127.0.0.1", true},
        {"127.0.0.2/31", "127.0.0.2", true},
        {"127.0.0.2/31", "127.0.0.3", true},
        {"12.34.56.78/23", "12.34.57.255", true},
        {"12.34.56.78/23", "12.34.56.0", true},
        {"12.34.56.78/25", "12.34.56.0", true},
        {"12.34.56.78/25", "12.34.56.127", true},
        {"::1", "::1", true},
        {"::1/96", "::1", true},
        {"::1/96", "::DEAD:BEEF", true},
        {"::1/128", "::1", true},
        {"::2/127", "::2", true},
        {"::2/127", "::3", true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:def0:1234:5678:9abc:def0",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:def0::", true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:def1:8888:7777:6666:5555",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:def2::", true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:def3::1", true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:def3:ffff:ffff:ffff:ffff",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/123",
         "1234:5678:9abc:def0:1234:5678:9abc:deef",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:def0",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:def1",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:def2",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:defe",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:deff",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/125",
         "1234:5678:9abc:def0:1234:5678:9abc:def7",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/126",
         "1234:5678:9abc:def0:1234:5678:9abc:def3",
         true},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/127",
         "1234:5678:9abc:def0:1234:5678:9abc:def1",
         true},

        {"127.0.0.1/8", "0.0.0.1", false},
        {"127.0.0.1/8", "123.45.67.89", false},
        {"127.0.0.1/8", "255.0.0.1", false},
        {"127.0.0.1", "127.0.0.2", false},
        {"127.0.0.2/31", "127.0.0.0", false},
        {"127.0.0.2/31", "127.0.0.1", false},
        {"12.34.56.78/23", "12.34.58.0", false},
        {"12.34.56.78/23", "12.34.55.255", false},
        {"12.34.56.78/25", "12.34.56.128", false},
        {"12.34.56.78/25", "12.34.55.255", false},
        {"::1", "::2", false},
        {"::1/96", "::1:0:0", false},
        {"::1/96", "DEAD::BEEF", false},
        {"::1/128", "8000::", false},
        {"::2/127", "::0", false},
        {"::2/127", "::1", false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:dee0:1234:5678:9abc:def0",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:dee0::", false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:dee1:8888:7777:6666:5555",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:dee2::", false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62", "1234:5678:9abc:dee3::1", false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/62",
         "1234:5678:9abc:dee3:ffff:ffff:ffff:ffff",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/123",
         "1234:5678:9abc:def0:1234:5678:9abc:dedf",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:deef",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/124",
         "1234:5678:9abc:def0:1234:5678:9abc:df01",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/125",
         "1234:5678:9abc:def0:1234:5678:9abc:def8",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/126",
         "1234:5678:9abc:def0:1234:5678:9abc:def4",
         false},
        {"1234:5678:9abc:def0:1234:5678:9abc:def0/127",
         "1234:5678:9abc:def0:1234:5678:9abc:def2",
         false},
    };
    for (auto&& p : test_data) {
        ASSERT_EQ(CIDR(p.range).contains(CIDR(p.value)), p.should_match);
    }
}

TEST(CIDRTest, containsBits) {
    // Check 255.255.255.255 against 0.0.0.0, 128.0.0.0, ..., 255.255.255.254, 255.255.255.255
    // For mask lengths ranging form 0 to 32
    for (int mask = 0; mask <= 32; ++mask) {
        CIDR range("255.255.255.255/" + std::to_string(mask));
        ASSERT_EQ(range.contains(CIDR("0.0.0.0")), mask == 0);
        for (int iplen = 1; iplen <= 32; ++iplen) {
            auto const ip = (0xFFFFFFFF << (32 - iplen)) & 0xFFFFFFFF;
            std::ostringstream oss;
            oss << ((ip >> 24) & 0xFF) << '.' << ((ip >> 16) & 0xFF) << '.' << ((ip >> 8) & 0xFF)
                << '.' << (ip & 0xFF);
            ASSERT_EQ(range.contains(CIDR(oss.str())), mask <= iplen);
        }
    }

    // IPv6 version of above
    for (int mask = 0; mask <= 128; ++mask) {
        CIDR range("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/" + std::to_string(mask));
        for (int iplen = 0; iplen <= 128; ++iplen) {
            std::ostringstream oss;
            for (int i = 0; i < 8; ++i) {
                if (iplen <= (i * 16)) {
                    oss << "::";
                    break;
                }

                if (i > 0) {
                    oss << ':';
                }

                if (iplen >= ((i + 1) * 16)) {
                    oss << "ffff";
                } else {
                    auto const bits = (0xFFFF << (16 - (iplen % 16))) & 0xFFFF;
                    oss << std::hex << bits;
                }
            }
            ASSERT_EQ(range.contains(CIDR(oss.str())), mask <= iplen);
        }
    }
}

const auto kBadIp = "Invalid IP address in CIDR string";
const auto kLenOOB = "Invalid length in CIDR string";
const auto kBadLen = "Non-numeric length in CIDR string";

TEST(CIDRTest, doesNotParse) {
    string_pair bad_addrs[] = {
        {"1.2.3.4.5", kBadIp},
        {"1.2.3", kBadIp},
        {"1::2::3", kBadIp},
        {"127.0.0.1/33", kLenOOB},
        {"::1/129", kLenOOB},
        {"1.2.3.4/-1", kLenOOB},
        {"::1/-1", kLenOOB},
        {"::1/flower", kBadLen},
        {"::1/12a", kBadLen},
        {"", kBadIp},
        {"/", kBadIp},
        {"1.2.3.4//", kBadLen},
        {"::/", kBadLen},
        {"candygram", kBadIp},
    };
    for (auto&& p : bad_addrs) {
        ASSERT_THROWS_WHAT(CIDR(p.first), DBException, p.second);
    }
}

}  // namespace
}  // namespace mongo
