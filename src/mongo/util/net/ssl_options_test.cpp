/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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

#include "mongo/util/net/ssl_options.h"

#include <ostream>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace test {
struct Vector : public std::vector<uint8_t> {
    Vector(std::vector<uint8_t> v) : std::vector<uint8_t>(std::move(v)) {}
};
std::ostream& operator<<(std::ostream& ss, const Vector& val) {
    ss << '{';
    std::string comma;
    for (const auto& b : val) {
        ss << comma << b;
        comma = ", ";
    }
    ss << '}';
    return ss;
}
}  // namespace test

TEST(SSLOptions, validCases) {
    SSLParams::CertificateSelector selector;

    ASSERT_OK(parseCertificateSelector(&selector, "subj", "subject=test.example.com"));
    ASSERT_EQ(selector.subject, "test.example.com");

    ASSERT_OK(parseCertificateSelector(&selector, "hash", "thumbprint=0123456789"));
    ASSERT_EQ(test::Vector(selector.thumbprint), test::Vector({0x01, 0x23, 0x45, 0x67, 0x89}));
}

TEST(SSLOptions, invalidCases) {
    SSLParams::CertificateSelector selector;

    auto status = parseCertificateSelector(&selector, "option", "bogus=nothing");
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(), "Unknown certificate selector property for 'option': 'bogus'");

    status = parseCertificateSelector(&selector, "option", "thumbprint=0123456");
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(),
              "Invalid certificate selector value for 'option': Not an even number of hexits");

    status = parseCertificateSelector(&selector, "option", "thumbprint=bogus");
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(),
              "Invalid certificate selector value for 'option': Not a valid hex string");
}

}  // namespace
}  // namespace mongo
