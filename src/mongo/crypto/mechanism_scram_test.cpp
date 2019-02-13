/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace scram {
namespace {

template <typename HashBlock>
void testBasicVectors(StringData saltedPw,
                      StringData clientKey,
                      StringData storedKey,
                      StringData serverKey,
                      StringData proof,
                      StringData signature) {
    // Predictable salts yield predictable secrets.
    // salt = {0, 1, 2, 3, ..., n-1}
    std::vector<uint8_t> salt;
    salt.resize(HashBlock::kHashLength - 4);
    int i = 0;
    std::generate(salt.begin(), salt.end(), [&i] { return i++; });

    Presecrets<HashBlock> presecrets("password", salt, 4096);
    ASSERT_EQ(presecrets.generateSaltedPassword().toString(), saltedPw);

    Secrets<HashBlock> secrets(presecrets);
    ASSERT_EQ(secrets.clientKey().toString(), clientKey);
    ASSERT_EQ(secrets.storedKey().toString(), storedKey);
    ASSERT_EQ(secrets.serverKey().toString(), serverKey);

    const StringData authMessage("secret");
    const auto generatedProof = secrets.generateClientProof(authMessage);
    ASSERT_EQ(generatedProof, proof);
    ASSERT_TRUE(secrets.verifyClientProof(authMessage, base64::decode(generatedProof)));

    const auto generatedSig = secrets.generateServerSignature(authMessage);
    ASSERT_EQ(generatedSig, signature);
    ASSERT_TRUE(secrets.verifyServerSignature(authMessage, base64::decode(generatedSig)));
}

TEST(MechanismScram, BasicVectors) {
    testBasicVectors<SHA1Block>("531aYHrF581Skow4E0gCWLw/Ibo=",
                                "wiHbIsPcvJo230S6Qf5xYCDrhb0=",
                                "SjXiaB2hLRr8aMUyXMVEw7H1jSI=",
                                "FilAoFIclBukd3xZxBvYMXTU3HM=",
                                "y+cpoAm0YlN30GuNgN4B9xghi4E=",
                                "kiZS90Kz4/yaYZn9JieHtcRzXR0=");
    testBasicVectors<SHA256Block>("UA7rgIQG0u7EQJuOrJ99qaWVlcWnY0e/ijWBuyzSN0M=",
                                  "xdYqTeBpV5U7m/j9EdpKT1Ls+5ublIEeYGND2RUB18k=",
                                  "w4nwnR0Mck11lMY3EeF4pCcpJMgaToIguPbEk/ipNGY=",
                                  "oKgZqeFO8FDpB14Y8QDLbiX1TurT6XZTdlexUt/Ny5g=",
                                  "D6x37wuGhm1HegzIrJhedSb26XOdg5IRyR47oFqzKIo=",
                                  "ybHsTJuRLmeT0/1YvQZKrlsgDE40RobAX7o8fu9sbdk=");
}

template <typename HashBlock>
void testGenerateCredentials() {
    const auto bson = Secrets<HashBlock>::generateCredentials("password", 4096);

    ASSERT_EQ(bson.nFields(), 4);

    ASSERT_TRUE(bson.hasField("salt"));
    ASSERT_EQ(base64::decode(bson.getStringField("salt")).size(), HashBlock::kHashLength - 4);

    ASSERT_TRUE(bson.hasField("storedKey"));
    ASSERT_EQ(base64::decode(bson.getStringField("storedKey")).size(), HashBlock::kHashLength);

    ASSERT_TRUE(bson.hasField("serverKey"));
    ASSERT_EQ(base64::decode(bson.getStringField("serverKey")).size(), HashBlock::kHashLength);

    ASSERT_TRUE(bson.hasField("iterationCount"));
    ASSERT_EQ(bson.getIntField("iterationCount"), 4096);
}

TEST(MechanismScram, generateCredentials) {
    testGenerateCredentials<SHA1Block>();
    testGenerateCredentials<SHA256Block>();
}

}  // namespace
}  // namespace scram
}  // namespace mongo
