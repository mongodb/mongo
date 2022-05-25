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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(SetShardVersionRequestTest, ParseFull) {
    const ChunkVersion chunkVersion(1, 2, OID::gen(), Timestamp(1, 1));

    SetShardVersionRequest request = assertGet(SetShardVersionRequest::parseFromBSON([&] {
        BSONObjBuilder builder(BSON("setShardVersion"
                                    << "db.coll"));
        chunkVersion.serializeToBSON("version", &builder);
        return builder.obj();
    }()));

    ASSERT(!request.shouldForceRefresh());
    ASSERT(!request.isAuthoritative());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequestTest, ParseFullWithAuthoritative) {
    const ChunkVersion chunkVersion(1, 2, OID::gen(), Timestamp(1, 1));

    SetShardVersionRequest request = assertGet(SetShardVersionRequest::parseFromBSON([&] {
        BSONObjBuilder builder(BSON("setShardVersion"
                                    << "db.coll"
                                    << "authoritative" << true));
        chunkVersion.serializeToBSON("version", &builder);
        return builder.obj();
    }()));

    ASSERT(!request.shouldForceRefresh());
    ASSERT(request.isAuthoritative());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequestTest, ParseFullNoNS) {
    const ChunkVersion chunkVersion(1, 2, OID::gen(), Timestamp(1, 1));

    auto ssvStatus = SetShardVersionRequest::parseFromBSON([&] {
        BSONObjBuilder builder(BSON("setShardVersion"
                                    << ""
                                    << "authoritative" << true));
        chunkVersion.serializeToBSON("version", &builder);
        return builder.obj();
    }());

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequestTest, ParseFullNSContainsDBOnly) {
    const ChunkVersion chunkVersion(1, 2, OID::gen(), Timestamp(1, 1));

    auto ssvStatus = SetShardVersionRequest::parseFromBSON([&] {
        BSONObjBuilder builder(BSON("setShardVersion"
                                    << "DBOnly"
                                    << "authoritative" << true));
        chunkVersion.serializeToBSON("version", &builder);
        return builder.obj();
    }());

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

}  // namespace
}  // namespace mongo
