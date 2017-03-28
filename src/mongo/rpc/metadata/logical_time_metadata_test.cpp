/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/unittest/unittest.h"

#include "mongo/util/log.h"

namespace mongo {
namespace rpc {
namespace {

TEST(LogicalTimeMetadataTest, Roundtrip) {
    const auto ts = LogicalTime(Timestamp(100, 200));

    SHA1Block::HashType proof;
    proof.fill(0);
    proof[19] = 6;
    proof[0] = 12;

    long long keyId = 1;

    SignedLogicalTime signedTs(LogicalTime(ts), proof, keyId);

    LogicalTimeMetadata origMetadata(signedTs);
    BSONObjBuilder builder;
    origMetadata.writeToMetadata(&builder);

    auto serializedObj = builder.done();
    auto parseStatus = LogicalTimeMetadata::readFromMetadata(serializedObj);
    ASSERT_OK(parseStatus.getStatus());

    const auto& parsedMetadata = parseStatus.getValue();
    const auto& parsedTs = parsedMetadata.getSignedTime();
    ASSERT_EQ(ts.asTimestamp(), parsedTs.getTime().asTimestamp());
    ASSERT_TRUE(SHA1Block(proof) == parsedTs.getProof());
    ASSERT_TRUE(keyId == parsedTs.getKeyId());
}

TEST(LogicalTimeMetadataTest, MissingClusterTimeShouldFailToParse) {
    std::array<uint8_t, 20> proof;
    proof.fill(0);

    long long keyId = 1;

    BSONObjBuilder builder;
    BSONObjBuilder subObjBuilder(builder.subobjStart("logicalTime"));
    BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart("signature"));
    signatureObjBuilder.append("hash", BSONBinData(proof.data(), proof.size(), BinDataGeneral));
    signatureObjBuilder.append("keyId", keyId);
    signatureObjBuilder.doneFast();
    subObjBuilder.doneFast();

    auto serializedObj = builder.done();
    auto status = LogicalTimeMetadata::readFromMetadata(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(LogicalTimeMetadataTest, MissingSignatureShouldFailToParse) {
    const auto ts = Timestamp(100, 200);

    BSONObjBuilder builder;
    BSONObjBuilder subObjBuilder(builder.subobjStart("logicalTime"));
    ts.append(subObjBuilder.bb(), "clusterTime");
    subObjBuilder.doneFast();

    auto serializedObj = builder.done();
    auto status = LogicalTimeMetadata::readFromMetadata(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(LogicalTimeMetadataTest, MissingHashShouldFailToParse) {
    const auto ts = Timestamp(100, 200);

    long long keyId = 1;

    BSONObjBuilder builder;
    BSONObjBuilder subObjBuilder(builder.subobjStart("logicalTime"));
    ts.append(subObjBuilder.bb(), "clusterTime");
    BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart("signature"));
    signatureObjBuilder.append("keyId", keyId);
    signatureObjBuilder.doneFast();
    subObjBuilder.doneFast();

    auto serializedObj = builder.done();
    auto status = LogicalTimeMetadata::readFromMetadata(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(LogicalTimeMetadataTest, MissingKeyIdShouldFailToParse) {
    const auto ts = Timestamp(100, 200);

    std::array<uint8_t, 20> proof;
    proof.fill(0);

    BSONObjBuilder builder;
    BSONObjBuilder subObjBuilder(builder.subobjStart("logicalTime"));
    ts.append(subObjBuilder.bb(), "clusterTime");
    BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart("signature"));
    signatureObjBuilder.append("hash", BSONBinData(proof.data(), proof.size(), BinDataGeneral));
    signatureObjBuilder.doneFast();
    subObjBuilder.doneFast();

    auto serializedObj = builder.done();
    auto status = LogicalTimeMetadata::readFromMetadata(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(LogicalTimeMetadataTest, ProofWithWrongLengthShouldFailToParse) {
    const auto ts = Timestamp(100, 200);

    std::array<uint8_t, 10> proof;
    proof.fill(0);

    long long keyId = 1;

    BSONObjBuilder builder;
    BSONObjBuilder subObjBuilder(builder.subobjStart("logicalTime"));
    ts.append(subObjBuilder.bb(), "clusterTime");
    BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart("signature"));
    signatureObjBuilder.append("hash", BSONBinData(proof.data(), proof.size(), BinDataGeneral));
    signatureObjBuilder.append("keyId", keyId);
    signatureObjBuilder.doneFast();
    subObjBuilder.doneFast();

    auto serializedObj = builder.done();
    auto status = LogicalTimeMetadata::readFromMetadata(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, status);
}

}  // namespace rpc
}  // namespace mongo
}
