/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/executor/connection_pool_tl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <vector>

namespace mongo::executor::connection_pool_tl {
namespace {

const HostAndPort kRemote("forged-peer", 27017);

// A forged hello reply advertising only PLAIN must yield no usable mechanisms and an error, so that
// connection setup fails rather than downgrading to a cleartext keyfile exchange (SERVER-130264).
TEST(FilterInternalAuthSaslMechsTest, PlainOnlyIsRejected) {
    std::vector<std::string> mechs;
    auto reply = BSON("saslSupportedMechs" << BSON_ARRAY(std::string{auth::kMechanismSaslPlain}));
    auto status = filterInternalAuthSaslMechs(reply, kRemote, &mechs);
    ASSERT_EQ(status.code(), ErrorCodes::AuthenticationFailed);
    ASSERT_TRUE(mechs.empty());
}

// When PLAIN is mixed with an allowlisted mechanism, only the allowlisted one is kept.
TEST(FilterInternalAuthSaslMechsTest, PlainIsFilteredOutButScramKept) {
    std::vector<std::string> mechs;
    auto reply =
        BSON("saslSupportedMechs" << BSON_ARRAY(std::string{auth::kMechanismSaslPlain}
                                                << std::string{auth::kMechanismScramSha256}));
    auto status = filterInternalAuthSaslMechs(reply, kRemote, &mechs);
    ASSERT_OK(status);
    ASSERT_EQ(mechs.size(), 1u);
    ASSERT_EQ(mechs[0], std::string{auth::kMechanismScramSha256});
}

// A non-array saslSupportedMechs (e.g. a bare string, or a missing field) is not interpreted as an
// advertised mechanism list: nothing is stored and no error is raised. Such a reply cannot seed a
// PLAIN downgrade here; the connection instead falls through to the standard negotiation path,
// where PLAIN is blocked by getInternalAuthParams (defence in depth).
TEST(FilterInternalAuthSaslMechsTest, NonArrayIsIgnored) {
    std::vector<std::string> mechs;
    auto reply = BSON("saslSupportedMechs" << std::string{auth::kMechanismSaslPlain});
    auto status = filterInternalAuthSaslMechs(reply, kRemote, &mechs);
    ASSERT_OK(status);
    ASSERT_TRUE(mechs.empty());
}

// A reply with no saslSupportedMechs field at all is a no-op.
TEST(FilterInternalAuthSaslMechsTest, MissingFieldIsIgnored) {
    std::vector<std::string> mechs;
    auto status = filterInternalAuthSaslMechs(BSONObj(), kRemote, &mechs);
    ASSERT_OK(status);
    ASSERT_TRUE(mechs.empty());
}

// The result is derived solely from the reply: any pre-existing entries (e.g. a stale "PLAIN") are
// discarded, not retained.
TEST(FilterInternalAuthSaslMechsTest, PriorContentsAreReplaced) {
    std::vector<std::string> mechs{std::string{auth::kMechanismSaslPlain}};
    auto reply = BSON("saslSupportedMechs" << BSON_ARRAY(std::string{auth::kMechanismScramSha256}));
    auto status = filterInternalAuthSaslMechs(reply, kRemote, &mechs);
    ASSERT_OK(status);
    ASSERT_EQ(mechs.size(), 1u);
    ASSERT_EQ(mechs[0], std::string{auth::kMechanismScramSha256});
}

}  // namespace
}  // namespace mongo::executor::connection_pool_tl
