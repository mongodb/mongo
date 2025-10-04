/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/router_role_api/mock_ns_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

ChunkRange parseRange(const BSONObj& query) {
    const StringData fieldName(query.firstElement().fieldName());

    if (query.firstElement().isNumber()) {
        return {BSON(fieldName << query.firstElement().numberInt()),
                BSON(fieldName << query.firstElement().numberInt() + 1)};
    } else if (query.firstElement().type() == BSONType::object) {
        BSONObj queryRange = query.firstElement().Obj();

        ASSERT(!queryRange[GTE.label()].eoo());
        ASSERT(!queryRange[LT.label()].eoo());

        BSONObjBuilder minKeyB;
        minKeyB.appendAs(queryRange[GTE.label()], fieldName);
        BSONObjBuilder maxKeyB;
        maxKeyB.appendAs(queryRange[LT.label()], fieldName);

        return {minKeyB.obj(), maxKeyB.obj()};
    }

    FAIL("Invalid query");
    MONGO_UNREACHABLE;
}

}  // namespace

MockNSTargeter::MockNSTargeter(const NamespaceString& nss, std::vector<MockRange> mockRanges)
    : _nss(nss), _mockRanges(std::move(mockRanges)) {
    ASSERT(_nss.isValid());
    ASSERT(!_mockRanges.empty());
}

std::vector<ShardEndpoint> MockNSTargeter::_targetQuery(const BSONObj& query) const {
    const ChunkRange queryRange(parseRange(query));

    std::vector<ShardEndpoint> endpoints;

    for (const auto& range : _mockRanges) {
        if (queryRange.overlapWith(range.range)) {
            endpoints.push_back(range.endpoint);
        }
    }

    uassert(ErrorCodes::UnknownError, "no mock ranges found for query", !endpoints.empty());
    return endpoints;
}

void assertEndpointsEqual(const ShardEndpoint& endpointA, const ShardEndpoint& endpointB) {
    ASSERT_EQUALS(endpointA.shardName, endpointB.shardName);
    ASSERT_EQUALS(endpointA.shardVersion->placementVersion().toLong(),
                  endpointB.shardVersion->placementVersion().toLong());
    ASSERT_EQUALS(endpointA.shardVersion->placementVersion().epoch(),
                  endpointB.shardVersion->placementVersion().epoch());
}

}  // namespace mongo
