// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/router_role/routing_context.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <exception>
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
boost::optional<Timestamp> getEffectiveAtClusterTime(OperationContext* opCtx) {
    if (auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        // Check the read concern for the atClusterTime argument.
        return atClusterTime->asTimestamp();
    }

    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        // Check to see if we're running in a transaction with snapshot level read concern.
        if (auto atClusterTime = txnRouter.getSelectedAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
    }

    // Otherwise, the latest routing table is sufficient.
    return boost::none;
}
}  // namespace

RoutingContext::RoutingContext(OperationContext* opCtx,
                               const std::vector<NamespaceString>& nssList,
                               bool allowLocks,
                               bool checkTimeseriesBucketsNss)
    : _catalogCache(Grid::get(opCtx)->catalogCache()),
      _checkTimeseriesBucketsNss(checkTimeseriesBucketsNss) {
    _nssRoutingInfoMap.reserve(nssList.size());

    for (const auto& nss : nssList) {
        auto insertRoutingInfo = [&](NamespaceString nss, CollectionRoutingInfo cri) {
            auto [it, inserted] = _nssRoutingInfoMap.try_emplace(
                nss, RoutingInfoEntry{std::move(cri), false /* validated */});
            tassert(10292300,
                    str::stream() << "Namespace " << nss.toStringForErrorMsg()
                                  << " declared multiple times in RoutingContext",
                    inserted);
        };

        auto cri = uassertStatusOK(_getCollectionRoutingInfo(opCtx, nss, allowLocks));

        // For tracked legacy timeseries collection we only register the buckets collection in the
        // global catalog, therefore we only need to check when the logical namespace does not have
        // a routing table.
        if (checkTimeseriesBucketsNss && !cri.getChunkManager().hasRoutingTable()) {
            auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
            const auto& bucketsCri =
                uassertStatusOK(_getCollectionRoutingInfo(opCtx, bucketsNss, allowLocks));
            if (bucketsCri.hasRoutingTable()) {
                insertRoutingInfo(bucketsNss, bucketsCri);
            }
        }

        insertRoutingInfo(nss, std::move(cri));
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
    : _catalogCache(nullptr), _nssRoutingInfoMap([nssMap = std::move(nssMap)]() {
          NssRoutingInfoMap result;
          for (auto&& [nss, cri] : nssMap) {
              result.emplace(std::move(nss),
                             RoutingInfoEntry{std::move(cri), false /* validated */});
          }
          return result;
      }()) {}

const CollectionRoutingInfo& RoutingContext::getCollectionRoutingInfo(
    const NamespaceString& nss) const {
    tassert(10411401,
            "should not have acquired routing info for namespace released from routing context",
            !_releasedNssList.contains(nss));
    auto it = _nssRoutingInfoMap.find(nss);
    uassert(10292301,
            str::stream() << "Attempted to access RoutingContext for undeclared namespace "
                          << nss.toStringForErrorMsg(),
            it != _nssRoutingInfoMap.end());
    return it->second.cri;
}

bool RoutingContext::hasNss(const NamespaceString& nss) const {
    return _nssRoutingInfoMap.find(nss) != _nssRoutingInfoMap.end();
}

std::vector<NamespaceString> RoutingContext::getNssList() const {
    std::vector<NamespaceString> nssList;
    nssList.reserve(_nssRoutingInfoMap.size());

    for (const auto& [nss, _] : _nssRoutingInfoMap) {
        nssList.emplace_back(nss);
    }

    return nssList;
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
    tassert(10411400,
            "should not have sent request for namespace released from routing context",
            !_releasedNssList.contains(nss));
    if (auto& validated = _nssRoutingInfoMap.at(nss).validated; !validated) {
        validated = true;
    }

    if (_checkTimeseriesBucketsNss) {
        auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
        if (_nssRoutingInfoMap.contains(bucketsNss)) {
            _nssRoutingInfoMap.at(bucketsNss).validated = true;
        }
    }
}

void RoutingContext::onStaleError(const Status& status,
                                  boost::optional<const NamespaceString&> nss) {
    tassert(11451300,
            str::stream() << "onStaleError called with invalid error category: " << status.code()
                          << ". Expected StaleShardVersionError or StaleDbVersion",
            status.code() == ErrorCodes::StaleDbVersion ||
                ErrorCodes::isStaleShardVersionError(status));

    sharding::router::onStaleError(
        _catalogCache, status, getNssList(), false /* filterUnrelatedNamespaces */, nss);
}

void RoutingContext::onStaleShardVersionError(const NamespaceString& nss,
                                              const boost::optional<ShardVersion>& wantedVersion) {
    _catalogCache->onStaleCollectionVersion(nss, wantedVersion);
}

void RoutingContext::skipValidation() {
    _skipValidation = true;
}

void RoutingContext::release(const NamespaceString& nss) {
    _releasedNssList.insert(nss);

    if (_checkTimeseriesBucketsNss) {
        auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
        if (_nssRoutingInfoMap.contains(bucketsNss)) {
            _releasedNssList.insert(bucketsNss);
        }
    }
}

void RoutingContext::validateOnContextEnd() const {
    if (_skipValidation) {
        return;
    }

    for (const auto& [nss, routingInfoEntry] : _nssRoutingInfoMap) {
        if (auto validated = routingInfoEntry.validated; !validated) {
            if (_releasedNssList.contains(nss)) {
                // If the namespace was released, we don't expect it to be validated.
                continue;
            }
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
