// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/crypto/mechanism_scram.h"

#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/unittest/unittest.h"

#include <cstring>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace scram {
namespace {

template <typename HashBlock>
void testBasicVectors(std::string_view saltedPw,
                      std::string_view clientKey,
                      std::string_view storedKey,
                      std::string_view serverKey,
                      std::string_view proof,
                      std::string_view signature) {
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

    const std::string_view authMessage("secret");
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
