// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/direct_shard_client_tracker.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"

#include <algorithm>

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
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bob;
        DirectShardClientTracker::get(opCtx).appendStats(&bob);
        return bob.obj();
    }
};

auto& directShardConnections =
    *ServerStatusSectionBuilder<DirectShardConnections>("directShardConnections").forShard();

}  // namespace

}  // namespace mongo
