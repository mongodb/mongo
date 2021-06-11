/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/oid.h"
#include "mongo/db/auth/security_token.h"
#include "mongo/db/auth/security_token_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/op_msg_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace rpc {
namespace test {
namespace {

constexpr auto kPingFieldName = "ping"_sd;
constexpr auto kTenantFieldName = "tenant"_sd;

class SecurityTokenMetadataTest : public ServiceContextTest {};

TEST_F(SecurityTokenMetadataTest, SecurityTokenNotAccepted) {
    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = BSON(kTenantFieldName << OID::gen());

    auth::gAcceptOpMsgSecurityToken = false;
    auto msgBytes = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody};
    ASSERT_THROWS_CODE_AND_WHAT(msgBytes.parse(),
                                DBException,
                                ErrorCodes::Unauthorized,
                                "Unsupported Security Token provided");
}

TEST_F(SecurityTokenMetadataTest, BasicSuccess) {
    const auto kOid = OID::gen();
    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = BSON(kTenantFieldName << kOid);

    auth::gAcceptOpMsgSecurityToken = true;
    auto msg = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody}.parse();
    ASSERT_BSONOBJ_EQ(msg.body, kPingBody);
    ASSERT_EQ(msg.sequences.size(), 0u);
    ASSERT_BSONOBJ_EQ(msg.securityToken, kTokenBody);

    auto opCtx = makeOperationContext();
    ASSERT(auth::getSecurityToken(opCtx.get()) == boost::none);

    auth::readSecurityTokenMetadata(opCtx.get(), msg.securityToken);
    auto token = auth::getSecurityToken(opCtx.get());
    ASSERT(token != boost::none);
    ASSERT_EQ(token->getTenant(), kOid);
}

}  // namespace
}  // namespace test
}  // namespace rpc
}  // namespace mongo
