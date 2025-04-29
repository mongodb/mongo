/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/s/routing_context.h"

#include <exception>
#include <optional>

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
boost::optional<Timestamp> getEffectiveAtClusterTime(OperationContext* opCtx) {
    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        // Check to see if we're running in a transaction with snapshot level read concern.
        if (auto atClusterTime = txnRouter.getSelectedAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
    }

    if (auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        // Check the read concern for the atClusterTime argument.
        return atClusterTime->asTimestamp();
    }

    // Otherwise, the latest routing table is sufficient.
    return boost::none;
}
}  // namespace

RoutingContext::RoutingContext(OperationContext* opCtx,
                               const std::vector<NamespaceString>& nssList,
                               bool allowLocks)
    : _catalogCache(Grid::get(opCtx)->catalogCache()) {
    _nssToCriMap.reserve(nssList.size());

    for (const auto& nss : nssList) {
        const auto cri = uassertStatusOK(_getCollectionRoutingInfo(opCtx, nss, allowLocks));
        auto [it, inserted] =
            _nssToCriMap.try_emplace(nss, std::make_pair(std::move(cri), boost::none));
        tassert(10292300,
                str::stream() << "Namespace " << nss.toStringForErrorMsg()
                              << " declared multiple times in RoutingContext",
                inserted);
    }
}

RoutingContext::RoutingContext(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& nssToCriMap)
    : _catalogCache(Grid::get(opCtx)->catalogCache()) {
    for (auto& [nss, cri] : nssToCriMap) {
        auto [it, inserted] =
            _nssToCriMap.try_emplace(nss, std::make_pair(std::move(cri), boost::none));
        tassert(10402001,
                str::stream() << "Namespace " << nss.toStringForErrorMsg()
                              << " declared multiple times in RoutingContext",
                inserted);
    }
}

RoutingContext RoutingContext::createForTest(
    stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap) {
    return RoutingContext(nssMap);
}
RoutingContext::RoutingContext(stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap)
    : _nssToCriMap([nssMap = std::move(nssMap)]() {
          NssCriMap result;
          for (auto&& [nss, cri] : nssMap) {
              result.emplace(std::move(nss), std::make_pair(std::move(cri), boost::none));
          }
          return result;
      }()) {}

const CollectionRoutingInfo& RoutingContext::getCollectionRoutingInfo(
    const NamespaceString& nss) const {
    auto it = _nssToCriMap.find(nss);
    uassert(10292301,
            str::stream() << "Attempted to access RoutingContext for undeclared namespace "
                          << nss.toStringForErrorMsg(),
            it != _nssToCriMap.end());
    return it->second.first;
}

StatusWith<CollectionRoutingInfo> RoutingContext::_getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) const {
    if (auto atClusterTime = getEffectiveAtClusterTime(opCtx)) {
        return _catalogCache->getCollectionRoutingInfoAt(opCtx, nss, *atClusterTime, allowLocks);
    } else {
        return _catalogCache->getCollectionRoutingInfo(opCtx, nss, allowLocks);
    }
}

void RoutingContext::onResponseReceivedForNss(const NamespaceString& nss, const Status& status) {
    if (auto& maybeStatus = _nssToCriMap.at(nss).second; !maybeStatus) {
        maybeStatus = status;
    }
}

bool RoutingContext::onStaleError(const NamespaceString& nss, const Status& status) {
    if (status.code() == ErrorCodes::StaleDbVersion) {
        auto si = status.extraInfo<StaleDbRoutingVersion>();
        // If the database version is stale, refresh its entry in the catalog cache.
        _catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
        return true;
    }

    if (ErrorCodes::isStaleShardVersionError(status)) {
        // 1. If the exception provides a shardId, add it to the set of shards requiring a refresh.
        // 2. If the cache currently considers the collection to be unsharded, this will trigger an
        //    epoch refresh.
        // 3. If no shard is provided, then the epoch is stale and we must refresh.
        if (auto si = status.extraInfo<StaleConfigInfo>()) {
            _catalogCache->onStaleCollectionVersion(si->getNss(), si->getVersionWanted());
        } else {
            _catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
        }
        return true;
    }

    uassertStatusOK(status);
    return false;
}

/**
 * Validate the RoutingContext on destruction to ensure that either:
 * 1. All declared namespaces have had their routing tables validated by sending a versioned
 * request to a shard. Each namespace should have a corresponding Status indicating the response
 * from the shard.
 * 2. An exception is thrown (i.e. if the collection generation has changed) and will be
 * propagated up the stack.
 *
 * It is considered a logic bug if a RoutingContext goes out of scope and neither of the above
 * are true.
 */
RoutingContext::~RoutingContext() {
    for (const auto& [nss, criPair] : _nssToCriMap) {
        auto maybeStatus = criPair.second;
        if (!maybeStatus) {
            LOGV2_ERROR(10292801,
                        "RoutingContext failed to validate routing table for all namespaces.",
                        "nss"_attr = nss.toStringForErrorMsg());
        }
    }
}
}  // namespace mongo
