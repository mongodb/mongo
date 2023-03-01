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

#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const std::string kPolicyName{"AutoMergerPolicy"};
}  // namespace


void AutoMergerPolicy::enable() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_enabled) {
        _enabled = true;
        _init(lk);
    }
}

void AutoMergerPolicy::disable() {
    stdx::lock_guard<Latch> lk(_mutex);
    _enabled = false;
    _collectionsToMergePerShard.clear();
}

bool AutoMergerPolicy::isEnabled() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _enabled;
}

void AutoMergerPolicy::checkInternalUpdates() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!feature_flags::gAutoMerger.isEnabled(serverGlobalParams.featureCompatibility) ||
        !_enabled) {
        return;
    }
    _checkInternalUpdatesWithLock(lk);
}

StringData AutoMergerPolicy::getName() const {
    return kPolicyName;
}

boost::optional<BalancerStreamAction> AutoMergerPolicy::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (!feature_flags::gAutoMerger.isEnabled(serverGlobalParams.featureCompatibility) ||
        !_enabled) {
        return boost::none;
    }

    _checkInternalUpdatesWithLock(lk);

    bool applyThrottling = false;

    if (_firstAction) {
        try {
            _collectionsToMergePerShard = _getNamespacesWithMergeableChunksPerShard(opCtx);
        } catch (DBException& e) {
            e.addContext("Failed to fetch collections with mergeable chunks");
            throw;
        }
        applyThrottling = true;
        _firstAction = false;
    }

    // Get next <shardId, collection> pair to merge
    for (auto it = _collectionsToMergePerShard.begin(); it != _collectionsToMergePerShard.end();) {
        auto& [shardId, collections] = *it;
        if (collections.empty()) {
            it = _collectionsToMergePerShard.erase(it);
            applyThrottling = true;
            continue;
        }

        auto mergeAction = MergeAllChunksOnShardInfo{shardId, collections.back()};
        mergeAction.applyThrottling = applyThrottling;

        collections.pop_back();
        return boost::optional<BalancerStreamAction>(mergeAction);
    }

    return boost::none;
}

void AutoMergerPolicy::applyActionResult(OperationContext* opCtx,
                                         const BalancerStreamAction& action,
                                         const BalancerStreamActionResponse& response) {
    stdx::visit(OverloadedVisitor{[&](const MergeAllChunksOnShardInfo& action) {
                                      const auto& status = stdx::get<Status>(response);
                                      if (!status.isOK()) {
                                          LOGV2_DEBUG(7312600,
                                                      1,
                                                      "Hit error while automerging chunks",
                                                      "shard"_attr = action.shardId,
                                                      "error"_attr = redact(status));
                                      }
                                  },
                                  [&](const DataSizeInfo& _) {
                                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                  },
                                  [&](const MigrateInfo& _) {
                                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                  },
                                  [&](const MergeInfo& _) {
                                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                  }},

                action);
}

void AutoMergerPolicy::_init(WithLock lk) {
    if (_enabled) {
        _intervalTimer.reset();
        _collectionsToMergePerShard.clear();
        _firstAction = true;
        _onStateUpdated();
    }
}

void AutoMergerPolicy::_checkInternalUpdatesWithLock(WithLock lk) {
    // Triggers automerger every `autoMergerIntervalSecs` seconds
    if (_collectionsToMergePerShard.empty() && _enabled &&
        _intervalTimer.seconds() > autoMergerIntervalSecs) {
        _init(lk);
    }
}

std::map<ShardId, std::vector<NamespaceString>>
AutoMergerPolicy::_getNamespacesWithMergeableChunksPerShard(OperationContext* opCtx) {
    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Fetch all the sharded collections which are not on defragmentation state
    std::map<ShardId, std::vector<NamespaceString>> collectionsToMerge;

    // TODO SERVER-73378 filter by collections with mergeable chunks
    std::vector<BSONObj> shardCollections =
        uassertStatusOK(configShard->exhaustiveFindOnConfig(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            repl::ReadConcernLevel::kMajorityReadConcern,
                            CollectionType::ConfigNS,
                            BSON(CollectionType::kDefragmentCollectionFieldName
                                 << BSON("$ne" << true) << CollectionType::kEnableAutoMergeFieldName
                                 << BSON("$ne" << false)),
                            BSONObj() /* no sort */,
                            boost::none /* no limit */))
            .docs;

    const auto& shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    for (const auto& shard : shardIds) {
        collectionsToMerge[shard].reserve(shardCollections.size());
        collectionsToMerge[shard] = [&] {
            std::vector<NamespaceString> collectionsOnShard;
            std::transform(shardCollections.begin(),
                           shardCollections.end(),
                           std::back_inserter(collectionsOnShard),
                           [](const BSONObj& doc) {
                               return NamespaceString(
                                   boost::none, doc.getStringField(CollectionType::kNssFieldName));
                           });
            return collectionsOnShard;
        }();
    }
    return collectionsToMerge;
};


}  // namespace mongo
