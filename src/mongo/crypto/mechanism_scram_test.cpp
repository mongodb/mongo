/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace scram {
namespace {

TEST(MechanismScram, BasicVectors) {
    const std::vector<uint8_t> kBadSha1Salt{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    ASSERT_EQ(kBadSha1Salt.size(), SHA1Block::kHashLength - 4);

    SHA1Presecrets presecrets("password", kBadSha1Salt, 4096);
    ASSERT_EQ(presecrets.generateSaltedPassword().toString(), "531aYHrF581Skow4E0gCWLw/Ibo=");

    SHA1Secrets secrets(presecrets);
    ASSERT_EQ(secrets.clientKey().toString(), "wiHbIsPcvJo230S6Qf5xYCDrhb0=");
    ASSERT_EQ(secrets.storedKey().toString(), "SjXiaB2hLRr8aMUyXMVEw7H1jSI=");
    ASSERT_EQ(secrets.serverKey().toString(), "FilAoFIclBukd3xZxBvYMXTU3HM=");

    const StringData authMessage("secret");
    auto proof = secrets.generateClientProof(authMessage);
    ASSERT_EQ(proof, "y+cpoAm0YlN30GuNgN4B9xghi4E=");
    ASSERT_TRUE(secrets.verifyClientProof(authMessage, base64::decode(proof)));

    auto sig = secrets.generateServerSignature(authMessage);
    ASSERT_EQ(sig, "kiZS90Kz4/yaYZn9JieHtcRzXR0=");
    ASSERT_TRUE(secrets.verifyServerSignature(authMessage, base64::decode(sig)));
}

TEST(MechanismScram, generateCredentials) {
    const auto bson = SHA1Secrets::generateCredentials("password", 4096);

    ASSERT_EQ(bson.nFields(), 4);

    ASSERT_TRUE(bson.hasField("salt"));
    ASSERT_EQ(base64::decode(bson.getStringField("salt")).size(), SHA1Block::kHashLength - 4);

    ASSERT_TRUE(bson.hasField("storedKey"));
    ASSERT_EQ(base64::decode(bson.getStringField("storedKey")).size(), SHA1Block::kHashLength);

    ASSERT_TRUE(bson.hasField("serverKey"));
    ASSERT_EQ(base64::decode(bson.getStringField("serverKey")).size(), SHA1Block::kHashLength);

    ASSERT_TRUE(bson.hasField("iterationCount"));
    ASSERT_EQ(bson.getIntField("iterationCount"), 4096);
}

}  // namespace
}  // namespace scram
}  // namespace mongo
