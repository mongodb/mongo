/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <algorithm>

#include "mongo/db/direct_shard_client_tracker.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"

namespace mongo {

namespace {

const auto getClientTracker = ServiceContext::declareDecoration<DirectShardClientTracker>();
const auto getClientToken =
    Client::declareDecoration<boost::optional<DirectShardClientTracker::Token>>();

}  // namespace

DirectShardClientTracker::Token::Token(ServiceContext* svcCtx) : _svcCtx(svcCtx) {
    auto& tracker = DirectShardClientTracker::get(_svcCtx);
    tracker._created.fetchAndAdd(1);
};

DirectShardClientTracker::Token::~Token() {
    auto& tracker = DirectShardClientTracker::get(_svcCtx);
    tracker._destroyed.fetchAndAdd(1);
}

DirectShardClientTracker& DirectShardClientTracker::get(ServiceContext* svcCtx) {
    return getClientTracker(svcCtx);
}

DirectShardClientTracker& DirectShardClientTracker::get(OperationContext* opCtx) {
    return getClientTracker(opCtx->getServiceContext());
}

void DirectShardClientTracker::trackClient(Client* client) {
    tassert(8255801,
            "Expected the client to be connected to the shard port",
            client->getService()->role().hasExclusively(ClusterRole::ShardServer));
    dassert(!getClientToken(client).has_value());
    getClientToken(client).emplace(client->getServiceContext());
}

void DirectShardClientTracker::appendStats(BSONObjBuilder* bob) const {
    auto created = _created.load();
    auto destroyed = _destroyed.load();
    // The 'created' and 'destroyed' counters above are not fetched atomically so there is a chance
    // that 'created' is less than than 'destroyed'.
    auto current = std::max(created - destroyed, 0LL);
    bob->append(kCurrentFieldName, current);
    bob->append(kCreatedFieldName, created);
}

namespace {

class DirectShardConnections : public ServerStatusSection {
public:
    DirectShardConnections() : ServerStatusSection("directShardConnections") {}

    bool includeByDefault() const override {
        return serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bob;
        DirectShardClientTracker::get(opCtx).appendStats(&bob);
        return bob.obj();
    }

} directShardConnections;

}  // namespace

}  // namespace mongo
