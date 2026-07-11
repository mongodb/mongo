// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/mock_ns_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

ChunkRange parseRange(const BSONObj& query) {
    const std::string_view fieldName(query.firstElement().fieldName());

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
