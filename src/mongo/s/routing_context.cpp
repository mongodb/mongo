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

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <exception>
#include <optional>

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
    _nssRoutingInfoMap.reserve(nssList.size());

    for (const auto& nss : nssList) {
        const auto cri = uassertStatusOK(_getCollectionRoutingInfo(opCtx, nss, allowLocks));
        auto [it, inserted] = _nssRoutingInfoMap.try_emplace(
            nss, RoutingInfoEntry{std::move(cri), false /* validated */});
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
        auto [it, inserted] = _nssRoutingInfoMap.try_emplace(
            nss, RoutingInfoEntry{std::move(cri), false /* validated */});
        tassert(10402001,
                str::stream() << "Namespace " << nss.toStringForErrorMsg()
                              << " declared multiple times in RoutingContext",
                inserted);
    }
}

std::unique_ptr<RoutingContext> RoutingContext::createSynthetic(
    stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap) {
    return std::unique_ptr<RoutingContext>(new RoutingContext(std::move(nssMap)));
}
RoutingContext::RoutingContext(stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap)
    : _nssRoutingInfoMap([nssMap = std::move(nssMap)]() {
          NssRoutingInfoMap result;
          for (auto&& [nss, cri] : nssMap) {
              result.emplace(std::move(nss),
                             RoutingInfoEntry{std::move(cri), false /* validated */});
          }
          return result;
      }()) {}

const CollectionRoutingInfo& RoutingContext::getCollectionRoutingInfo(
    const NamespaceString& nss) const {
    auto it = _nssRoutingInfoMap.find(nss);
    uassert(10292301,
            str::stream() << "Attempted to access RoutingContext for undeclared namespace "
                          << nss.toStringForErrorMsg(),
            it != _nssRoutingInfoMap.end());
    return it->second.cri;
}

StatusWith<CollectionRoutingInfo> RoutingContext::_getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) const {
    if (auto atClusterTime = getEffectiveAtClusterTime(opCtx)) {
        return _catalogCache->getCollectionRoutingInfoAt(opCtx, nss, *atClusterTime, allowLocks);
    } else {
        return _catalogCache->getCollectionRoutingInfo(opCtx, nss, allowLocks);
    }
}

void RoutingContext::onRequestSentForNss(const NamespaceString& nss) {
    if (auto& validated = _nssRoutingInfoMap.at(nss).validated; !validated) {
        validated = true;
    }
}

void RoutingContext::onStaleError(const NamespaceString& nss, const Status& status) {
    if (status.code() == ErrorCodes::StaleDbVersion) {
        auto si = status.extraInfo<StaleDbRoutingVersion>();
        // If the database version is stale, refresh its entry in the catalog cache.
        _catalogCache->onStaleDatabaseVersion(si->getDb(), si->getVersionWanted());
    } else if (ErrorCodes::isStaleShardVersionError(status)) {
        // 1. If the exception provides a shardId, add it to the set of shards requiring a refresh.
        // 2. If the cache currently considers the collection to be unsharded, this will trigger an
        //    epoch refresh.
        // 3. If no shard is provided, then the epoch is stale and we must refresh.
        if (auto si = status.extraInfo<StaleConfigInfo>()) {
            _catalogCache->onStaleCollectionVersion(si->getNss(), si->getVersionWanted());
        } else {
            _catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
        }
    }
}

void RoutingContext::skipValidation() {
    _skipValidation = true;
}

void RoutingContext::validateOnContextEnd() const {
    if (_skipValidation) {
        return;
    }

    for (const auto& [nss, routingInfoEntry] : _nssRoutingInfoMap) {
        if (auto validated = routingInfoEntry.validated; !validated) {
            if (TestingProctor::instance().isEnabled()) {
                tasserted(10446900,
                          str::stream()
                              << "RoutingContext ended without validating routing tables for nss "
                              << nss.toStringForErrorMsg());
            } else {
                LOGV2_ERROR(10446901,
                            "RoutingContext ended without validating routing tables for nss.",
                            "nss"_attr = nss.toStringForErrorMsg());
            }
        }
    }
}
}  // namespace mongo
