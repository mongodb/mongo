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


#include "mongo/util/dns_name.h"

#include "mongo/stdx/utility.h"
#include "mongo/unittest/unittest.h"

using namespace std::literals::string_literals;

namespace mongo {
namespace {

// To silence some warnings on some compilers at some aggressive warning levels, we use an "enum
// wrapper" struct which provides a legitimate implicit conversion from the checked value to the
// underlying type.
template <typename Enum>
struct Checked {
    static_assert(std::is_enum<Enum>::value, "Checked Value test data type must be an enum.");
    Enum value;
    Checked(Enum v) : value(v) {}
    using underlying_type = typename std::underlying_type<Enum>::type;
    operator underlying_type() const {
        return underlying_type(value);
    }
};


TEST(DNSNameTest, CorrectParsing) {
    enum FQDNBool : bool { kIsFQDN = true, kNotFQDN = false };
    const struct {
        std::string input;
        std::vector<std::string> parsedDomains;
        Checked<FQDNBool> isFQDN;
    } tests[] = {
        {"com."s, {"com"s}, kIsFQDN},
        {"com"s, {"com"s}, kNotFQDN},
        {"mongodb.com."s, {"com"s, "mongodb"s}, kIsFQDN},
        {"mongodb.com"s, {"com"s, "mongodb"s}, kNotFQDN},
        {"atlas.mongodb.com."s, {"com"s, "mongodb"s, "atlas"s}, kIsFQDN},
        {"atlas.mongodb.com"s, {"com"s, "mongodb"s, "atlas"s}, kNotFQDN},
        {"server.atlas.mongodb.com."s, {"com"s, "mongodb"s, "atlas"s, "server"s}, kIsFQDN},
        {"server.atlas.mongodb.com"s, {"com"s, "mongodb"s, "atlas"s, "server"s}, kNotFQDN},
    };

    for (const auto& test : tests) {
        const ::mongo::dns::HostName host(test.input);

        ASSERT_EQ(host.nameComponents().size(), test.parsedDomains.size());
        for (std::size_t i = 0; i < host.nameComponents().size(); ++i) {
            ASSERT_EQ(host.nameComponents()[i], test.parsedDomains[i]);
        }
        ASSERT(host.isFQDN() == test.isFQDN);
    }
}

TEST(DNSNameTest, CanonicalName) {
    const struct {
        std::string input;
        std::string result;
    } tests[] = {
        {"com."s, "com."s},
        {"com"s, "com"s},
        {"mongodb.com."s, "mongodb.com."s},
        {"mongodb.com"s, "mongodb.com"s},
        {"atlas.mongodb.com."s, "atlas.mongodb.com."s},
        {"atlas.mongodb.com"s, "atlas.mongodb.com"s},
        {"server.atlas.mongodb.com."s, "server.atlas.mongodb.com."s},
        {"server.atlas.mongodb.com"s, "server.atlas.mongodb.com"s},
    };

    for (const auto& test : tests) {
        const ::mongo::dns::HostName host(test.input);

        ASSERT_EQ(host.canonicalName(), test.result);
    }
}

TEST(DNSNameTest, NoncanonicalName) {
    const struct {
        std::string input;
        std::string result;
    } tests[] = {
        {"com."s, "com"s},
        {"com"s, "com"s},
        {"mongodb.com."s, "mongodb.com"s},
        {"mongodb.com"s, "mongodb.com"s},
        {"atlas.mongodb.com."s, "atlas.mongodb.com"s},
        {"atlas.mongodb.com"s, "atlas.mongodb.com"s},
        {"server.atlas.mongodb.com."s, "server.atlas.mongodb.com"s},
        {"server.atlas.mongodb.com"s, "server.atlas.mongodb.com"s},
    };

    for (const auto& test : tests) {
        const ::mongo::dns::HostName host(test.input);

        ASSERT_EQ(host.noncanonicalName(), test.result);
    }
}

TEST(DNSNameTest, Contains) {
    enum IsSubdomain : bool { kIsSubdomain = true, kNotSubdomain = false };
    enum TripsCheck : bool { kFailure = true, kSuccess = false };
    const struct {
        std::string domain;
        std::string subdomain;
        Checked<IsSubdomain> isSubdomain;
        Checked<TripsCheck> tripsCheck;
    } tests[] = {
        {"com."s, "mongodb.com."s, kIsSubdomain, kSuccess},
        {"com"s, "mongodb.com"s, kIsSubdomain, kFailure},
        {"com."s, "mongodb.com"s, kNotSubdomain, kFailure},
        {"com"s, "mongodb.com."s, kNotSubdomain, kFailure},

        {"com."s, "atlas.mongodb.com."s, kIsSubdomain, kSuccess},
        {"com"s, "atlas.mongodb.com"s, kIsSubdomain, kFailure},
        {"com."s, "atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"com"s, "atlas.mongodb.com."s, kNotSubdomain, kFailure},

        {"org."s, "atlas.mongodb.com."s, kNotSubdomain, kSuccess},
        {"org"s, "atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"org."s, "atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"org"s, "atlas.mongodb.com."s, kNotSubdomain, kFailure},

        {"com."s, "com."s, kNotSubdomain, kSuccess},
        {"com"s, "com."s, kNotSubdomain, kFailure},
        {"com."s, "com"s, kNotSubdomain, kFailure},
        {"com"s, "com"s, kNotSubdomain, kFailure},

        {"mongodb.com."s, "mongodb.com."s, kNotSubdomain, kSuccess},
        {"mongodb.com."s, "mongodb.com"s, kNotSubdomain, kFailure},
        {"mongodb.com"s, "mongodb.com."s, kNotSubdomain, kFailure},
        {"mongodb.com"s, "mongodb.com"s, kNotSubdomain, kFailure},

        {"mongodb.com."s, "atlas.mongodb.com."s, kIsSubdomain, kSuccess},
        {"mongodb.com"s, "atlas.mongodb.com"s, kIsSubdomain, kFailure},
        {"mongodb.com."s, "atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"mongodb.com"s, "atlas.mongodb.com."s, kNotSubdomain, kFailure},

        {"mongodb.com."s, "server.atlas.mongodb.com."s, kIsSubdomain, kSuccess},
        {"mongodb.com"s, "server.atlas.mongodb.com"s, kIsSubdomain, kFailure},
        {"mongodb.com."s, "server.atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"mongodb.com"s, "server.atlas.mongodb.com."s, kNotSubdomain, kFailure},

        {"mongodb.org."s, "server.atlas.mongodb.com."s, kNotSubdomain, kSuccess},
        {"mongodb.org"s, "server.atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"mongodb.org."s, "server.atlas.mongodb.com"s, kNotSubdomain, kFailure},
        {"mongodb.org"s, "server.atlas.mongodb.com."s, kNotSubdomain, kFailure},
    };

    for (const auto& test : tests) {
        const ::mongo::dns::HostName domain(test.domain);
        const ::mongo::dns::HostName subdomain(test.subdomain);

        try {
            ASSERT(test.isSubdomain == IsSubdomain(domain.contains(subdomain)));
            ASSERT(!test.tripsCheck);
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
            ASSERT(test.tripsCheck);
        }
    }
}

TEST(DNSNameTest, Resolution) {
    enum Failure : bool { kFails = true, kSucceeds = false };
    enum FQDNBool : bool { kIsFQDN = true, kNotFQDN = false };
    const struct {
        std::string domain;
        std::string subdomain;
        std::string result;

        Checked<Failure> fails;
        Checked<FQDNBool> isFQDN;
    } tests[] = {
        {"mongodb.com."s, "atlas"s, "atlas.mongodb.com."s, kSucceeds, kIsFQDN},
        {"mongodb.com"s, "atlas"s, "atlas.mongodb.com"s, kSucceeds, kNotFQDN},

        {"mongodb.com."s, "server.atlas"s, "server.atlas.mongodb.com."s, kSucceeds, kIsFQDN},
        {"mongodb.com"s, "server.atlas"s, "server.atlas.mongodb.com"s, kSucceeds, kNotFQDN},

        {"mongodb.com."s, "atlas."s, "FAILS"s, kFails, kNotFQDN},
        {"mongodb.com"s, "atlas."s, "FAILS"s, kFails, kNotFQDN},
    };

    for (const auto& test : tests) {
        try {
            const ::mongo::dns::HostName domain(test.domain);
            const ::mongo::dns::HostName subdomain(test.subdomain);
            const ::mongo::dns::HostName resolved = [&] {
                try {
                    const ::mongo::dns::HostName rv = subdomain.resolvedIn(domain);
                    return rv;
                } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
                    ASSERT(test.fails);
                    throw;
                }
            }();
            ASSERT(!test.fails);

            ASSERT_EQ(test.result, resolved.canonicalName());
            ASSERT(test.isFQDN == resolved.isFQDN());
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
            ASSERT(test.fails);
        }
    }
}

TEST(DNSNameTest, ForceQualification) {
    enum FQDNBool : bool { kIsFQDN = true, kNotFQDN = false };
    using Qualification = ::mongo::dns::HostName::Qualification;
    const struct {
        std::string domain;
        Checked<FQDNBool> startedFQDN;
        ::mongo::dns::HostName::Qualification forced;
        Checked<FQDNBool> becameFQDN;
        std::string becameCanonical;
    } tests[] = {
        {"mongodb.com."s, kIsFQDN, Qualification::kFullyQualified, kIsFQDN, "mongodb.com."s},
        {"mongodb.com"s, kNotFQDN, Qualification::kFullyQualified, kIsFQDN, "mongodb.com."s},

        {"atlas.mongodb.com."s,
         kIsFQDN,
         Qualification::kFullyQualified,
         kIsFQDN,
         "atlas.mongodb.com."s},
        {"atlas.mongodb.com"s,
         kNotFQDN,
         Qualification::kFullyQualified,
         kIsFQDN,
         "atlas.mongodb.com."s},

        {"mongodb.com."s, kIsFQDN, Qualification::kRelativeName, kNotFQDN, "mongodb.com"s},
        {"mongodb.com"s, kNotFQDN, Qualification::kRelativeName, kNotFQDN, "mongodb.com"s},

        {"atlas.mongodb.com."s,
         kIsFQDN,
         Qualification::kRelativeName,
         kNotFQDN,
         "atlas.mongodb.com"s},
        {"atlas.mongodb.com"s,
         kNotFQDN,
         Qualification::kRelativeName,
         kNotFQDN,
         "atlas.mongodb.com"s},
    };

    for (const auto& test : tests) {
        ::mongo::dns::HostName domain(test.domain);
        ASSERT(stdx::as_const(domain).isFQDN() == test.startedFQDN);
        domain.forceQualification(test.forced);
        ASSERT(stdx::as_const(domain).isFQDN() == test.becameFQDN);

        ASSERT_EQ(stdx::as_const(domain).canonicalName(), test.becameCanonical);
    }
}
}  // namespace
}  // namespace mongo
