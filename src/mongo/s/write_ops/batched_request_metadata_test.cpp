/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#include "mongo/s/write_ops/batched_request_metadata.h"

#include <string>

#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONArray;
using mongo::BSONArrayBuilder;
using mongo::BSONObj;
using mongo::BatchedRequestMetadata;
using mongo::OID;
using mongo::Timestamp;
using std::string;

TEST(RoundTrip, Normal) {
    // The BSON_ARRAY macro doesn't support Timestamps.
    BSONArrayBuilder arrBuilder;
    arrBuilder.append(Timestamp(1, 1));
    arrBuilder.append(OID::gen());
    BSONArray shardVersionArray = arrBuilder.arr();

    BSONObj metadataObj(BSON(BatchedRequestMetadata::shardName("shard0000")
                             << BatchedRequestMetadata::shardVersion() << shardVersionArray
                             << BatchedRequestMetadata::session(100)));

    string errMsg;
    BatchedRequestMetadata metadata;
    bool ok = metadata.parseBSON(metadataObj, &errMsg);
    ASSERT_TRUE(ok);

    BSONObj genMetadataObj = metadata.toBSON();
    ASSERT_EQUALS(0, genMetadataObj.woCompare(metadataObj));
}

}  // unnamed namespace
