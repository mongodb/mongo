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

#include "mongo/db/router_role/collection_routing_info_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/target_write_op.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(waitForDatabaseToBeDropped);

/**
 * Returns true if the two CollectionRoutingInfo objects are different.
 */
bool isMetadataDifferent(const CollectionRoutingInfo& criA, const CollectionRoutingInfo& criB) {
    if (criA.hasRoutingTable() != criB.hasRoutingTable())
        return true;

    if (criA.hasRoutingTable()) {
        return criA.getChunkManager().getVersion() != criB.getChunkManager().getVersion();
    }

    return criA.getDbVersion() != criB.getDbVersion();
}
}  // namespace

constexpr size_t kMaxDatabaseCreationAttempts = 3;

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             boost::optional<OID> targetEpoch)
    : _nss(nss),
      _targetEpoch(std::move(targetEpoch)),
      _routingCtx(_init(opCtx, false)),
      _cri(_routingCtx->getCollectionRoutingInfo(_nss)) {}

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(const NamespaceString& nss,
                                                             const RoutingContext& routingCtx)
    : _nss(nss), _cri(routingCtx.getCollectionRoutingInfo(nss)) {
    // TODO SERVER-106874 remove the namespace translation check entirely once 9.0 becomes last
    // LTS. By then we will only have viewless timeseries that do not require nss translation.
    auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
    if (routingCtx.hasNss(bucketsNss)) {
        _nss = bucketsNss;
        _cri = routingCtx.getCollectionRoutingInfo(_nss);
        _nssConvertedToTimeseriesBuckets = true;
    }
    _routingCtx = RoutingContext::createSynthetic({{_nss, _cri}});

    tassert(11428702,
            "Expected namespace from ChunkManager to match provided namespace",
            !_cri.hasRoutingTable() || _cri.getChunkManager().getNss() == _nss);
}

namespace {
std::unique_ptr<RoutingContext> createDatabasesAndGetRoutingCtxImpl(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& nssList,
    bool checkTimeseriesBucketsNss,
    bool refresh) {
    for (const auto& nss : nssList) {
        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "Must use a real namespace with CollectionRoutingInfoTargeter, got "
                          << nss.toStringForErrorMsg(),
            !nss.isCollectionlessAggregateNS());
    }

    // Scan 'nssList' and make a de-duplicated list of relevant databases.
    auto dbList = [&]() {
        std::vector<DatabaseName> dbList;
        std::set<DatabaseName> dedup;
        for (const auto& nss : nssList) {
            if (auto [it, inserted] = dedup.insert(nss.dbName()); inserted) {
                dbList.emplace_back(*it);
            }
        }

        return dbList;
    }();

    size_t attempts = 1;
    while (true) {
        try {
            // Ensure all the relevant databases exist.
            for (const auto& dbName : dbList) {
                cluster::createDatabase(opCtx, dbName);
            }

            if (refresh) {
                for (const auto& nss : nssList) {
                    Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(
                        nss, boost::none /* wantedVersion */);
                }
            }

            if (MONGO_unlikely(waitForDatabaseToBeDropped.shouldFail())) {
                LOGV2(8314600, "Hanging due to waitForDatabaseToBeDropped fail point");
                waitForDatabaseToBeDropped.pauseWhileSet(opCtx);
            }

            // Create a RoutingContext and return it. If the RoutingContext constructor fails,
            // an exception will be thrown.
            const auto allowLocks = opCtx->inMultiDocumentTransaction() &&
                shard_role_details::getLocker(opCtx)->isLocked();

            return std::make_unique<RoutingContext>(
                opCtx, nssList, allowLocks, checkTimeseriesBucketsNss);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            LOGV2_INFO(8314601,
                       "Failed initialization of routing info because the database has been "
                       "concurrently dropped",
                       "db"_attr = dbList,
                       "attemptNumber"_attr = attempts,
                       "maxAttempts"_attr = kMaxDatabaseCreationAttempts);

            if (attempts++ >= kMaxDatabaseCreationAttempts) {
                // The maximum number of attempts has been reached, so the procedure fails as it
                // could be a logical error. At this point, it is unlikely that the error is
                // caused by concurrent drop database operations.
                throw;
            }
        }
    }
}
}  // namespace

std::unique_ptr<RoutingContext> createDatabasesAndGetRoutingCtx(
    OperationContext* opCtx, const std::vector<NamespaceString>& nssList) {
    const bool checkTSBucketsNss = true;
    const bool refresh = false;

    return createDatabasesAndGetRoutingCtxImpl(opCtx, nssList, checkTSBucketsNss, refresh);
}

std::unique_ptr<RoutingContext> CollectionRoutingInfoTargeter::_createDatabaseAndGetRoutingCtx(
    OperationContext* opCtx, const NamespaceString& nss, bool refresh) {
    const bool checkTSBucketsNss = false;

    return createDatabasesAndGetRoutingCtxImpl(opCtx, {nss}, checkTSBucketsNss, refresh);
}

std::unique_ptr<RoutingContext> CollectionRoutingInfoTargeter::_init(OperationContext* opCtx,
                                                                     bool refresh) {
    auto routingCtx = _createDatabaseAndGetRoutingCtx(opCtx, _nss, refresh);
    const auto& cm = routingCtx->getCollectionRoutingInfo(_nss).getChunkManager();

    const auto checkStaleEpoch = [&](const ChunkManager& cm) {
        if (_targetEpoch) {
            uassert(StaleEpochInfo(_nss, ShardVersion{}, ShardVersion{}),
                    "Collection has been dropped",
                    cm.hasRoutingTable());
            uassert(StaleEpochInfo(_nss, ShardVersion{}, ShardVersion{}),
                    "Collection epoch has changed",
                    cm.getVersion().epoch() == *_targetEpoch);
        }
    };

    // For a tracked viewful time-series collection, only the underlying buckets collection is
    // stored on the config servers. If the user operation is on the time-series view namespace, we
    // should check if the buckets namespace is tracked on the configsvr. There are a few cases that
    // we need to take care of:
    // 1. The request is on the view namespace. We check if the buckets collection is tracked. If it
    //    is, we use the buckets collection namespace for the purpose of targeting. Additionally, we
    //    set the `_nssConvertedToTimeseriesBuckets` to true for this case.
    // 2. If request is on the buckets namespace, we don't need to execute any additional
    //    time-series logic. We can treat the request as though it was a request on a regular
    //    collection.
    // 3. During a cache refresh the buckets collection changes from tracked to untracked. In this
    //    case, if the original request is on the view namespace, then we should reset the namespace
    //    back to the view namespace and reset `_nssConvertedToTimeseriesBuckets`.
    //
    // TODO SERVER-106874 remove this if/else block entirely once 9.0 becomes last LTS. By then we
    // will only have viewless timeseries that do not require nss translation.
    if (!cm.hasRoutingTable() && !_nss.isTimeseriesBucketsCollection()) {
        auto bucketsNs = _nss.makeTimeseriesBucketsNamespace();
        auto bucketsRoutingCtx = _createDatabaseAndGetRoutingCtx(opCtx, bucketsNs, refresh);
        const auto& bucketsCri = bucketsRoutingCtx->getCollectionRoutingInfo(bucketsNs);
        if (bucketsCri.hasRoutingTable()) {
            _nss = bucketsNs;
            const auto& bucketsCm = bucketsCri.getChunkManager();
            _nssConvertedToTimeseriesBuckets = true;
            checkStaleEpoch(bucketsCm);
            return bucketsRoutingCtx;
        }
    } else if (!cm.hasRoutingTable() && _nssConvertedToTimeseriesBuckets) {
        // This can happen if a tracked time-series collection is dropped and re-created. Then we
        // need to reset the namespace to the original namespace.
        _nss = _nss.getTimeseriesViewNamespace();
        auto newRoutingCtx = _createDatabaseAndGetRoutingCtx(opCtx, _nss, refresh);
        const auto& newCm = newRoutingCtx->getCollectionRoutingInfo(_nss).getChunkManager();
        _nssConvertedToTimeseriesBuckets = false;
        checkStaleEpoch(newCm);
        return newRoutingCtx;
    }

    checkStaleEpoch(cm);
    return routingCtx;
}

const NamespaceString& CollectionRoutingInfoTargeter::getNS() const {
    return _nss;
}

ShardEndpoint CollectionRoutingInfoTargeter::targetInsert(OperationContext* opCtx,
                                                          const BSONObj& doc) const {
    return mongo::targetInsert(opCtx, _nss, _cri, _nssConvertedToTimeseriesBuckets, doc);
}

NSTargeter::TargetingResult CollectionRoutingInfoTargeter::targetUpdate(
    OperationContext* opCtx, const BatchItemRef& itemRef) const {
    auto result = mongo::targetUpdate(opCtx, _nss, _cri, _nssConvertedToTimeseriesBuckets, itemRef);

    return {std::move(result.endpoints),
            result.useTwoPhaseWriteProtocol,
            result.isNonTargetedRetryableWriteWithId};
}

NSTargeter::TargetingResult CollectionRoutingInfoTargeter::targetDelete(
    OperationContext* opCtx, const BatchItemRef& itemRef) const {
    auto result = mongo::targetDelete(opCtx, _nss, _cri, _nssConvertedToTimeseriesBuckets, itemRef);

    return {std::move(result.endpoints),
            result.useTwoPhaseWriteProtocol,
            result.isNonTargetedRetryableWriteWithId};
}

std::vector<ShardEndpoint> CollectionRoutingInfoTargeter::targetAllShards(
    OperationContext* opCtx) const {
    return mongo::targetAllShards(opCtx, _cri);
}

BSONObj CollectionRoutingInfoTargeter::extractBucketsShardKeyFromTimeseriesDoc(
    const BSONObj& doc,
    const ShardKeyPattern& pattern,
    const TimeseriesOptions& timeseriesOptions) {
    return mongo::extractBucketsShardKeyFromTimeseriesDoc(doc, pattern, timeseriesOptions);
}

bool CollectionRoutingInfoTargeter::isExactIdQuery(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& collation,
                                                   const ChunkManager& cm) {
    return mongo::isExactIdQuery(
        opCtx, nss, query, collation, cm.isSharded(), cm.getDefaultCollator());
}

void CollectionRoutingInfoTargeter::noteCouldNotTarget() {
    dassert(!_lastError || _lastError.value() == LastErrorType::kCouldNotTarget);
    _lastError = LastErrorType::kCouldNotTarget;
}

void CollectionRoutingInfoTargeter::noteStaleCollVersionResponse(OperationContext* opCtx,
                                                                 const StaleConfigInfo& staleInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kStaleShardVersion);
    Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(staleInfo.getNss(),
                                                               staleInfo.getVersionWanted());

    if (staleInfo.getNss() != _nss) {
        // This can happen when a time-series collection becomes sharded.
        Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(_nss, boost::none);
    }

    _lastError = LastErrorType::kStaleShardVersion;
}

void CollectionRoutingInfoTargeter::noteStaleDbVersionResponse(
    OperationContext* opCtx, const StaleDbRoutingVersion& staleInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kStaleDbVersion);
    Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(_nss.dbName(),
                                                             staleInfo.getVersionWanted());
    _lastError = LastErrorType::kStaleDbVersion;
}

bool CollectionRoutingInfoTargeter::hasStaleShardResponse() {
    return _lastError &&
        (_lastError.value() == LastErrorType::kStaleShardVersion ||
         _lastError.value() == LastErrorType::kStaleDbVersion);
}

void CollectionRoutingInfoTargeter::noteCannotImplicitlyCreateCollectionResponse(
    OperationContext* opCtx, const CannotImplicitlyCreateCollectionInfo& createInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kCannotImplicitlyCreateCollection);

    // TODO (SERVER-82939) Remove this check once the namespaces are guaranteed to match.
    //
    // In the case that a bulk write is performing operations on two different namespaces, a
    // CannotImplicitlyCreateCollection error for one namespace can be duplicated to operations on
    // the other namespace. In this case, we only need to create the collection for the namespace
    // the error actually refers to.
    if (createInfo.getNss() == _nss) {
        _lastError = LastErrorType::kCannotImplicitlyCreateCollection;
    }
}

bool CollectionRoutingInfoTargeter::refreshIfNeeded(OperationContext* opCtx) {
    // Did we have any stale config or targeting errors at all?
    if (!_lastError) {
        return false;
    }

    // Make sure that even in case of exception we will clear the last error.
    ON_BLOCK_EXIT([&] { _lastError = boost::none; });

    LOGV2_DEBUG(22912,
                4,
                "CollectionRoutingInfoTargeter checking if refresh is needed",
                "couldNotTarget"_attr = _lastError.value() == LastErrorType::kCouldNotTarget,
                "staleShardVersion"_attr = _lastError.value() == LastErrorType::kStaleShardVersion,
                "staleDbVersion"_attr = _lastError.value() == LastErrorType::kStaleDbVersion);

    // Get the latest metadata information from the cache if there were issues
    auto lastManager = _cri;
    _routingCtx = _init(opCtx, /*refresh*/ false);
    _cri = _routingCtx->getCollectionRoutingInfo(_nss);
    auto metadataChanged = isMetadataDifferent(lastManager, _cri);

    if (_lastError.value() == LastErrorType::kCouldNotTarget && !metadataChanged) {
        // If we couldn't target, and we didn't already update the metadata we must force a refresh.
        _routingCtx = _init(opCtx, /*refresh*/ true);
        _cri = _routingCtx->getCollectionRoutingInfo(_nss);
        metadataChanged = isMetadataDifferent(lastManager, _cri);
    }

    return metadataChanged;
}

bool CollectionRoutingInfoTargeter::createCollectionIfNeeded(OperationContext* opCtx) {
    if (!_lastError || _lastError != LastErrorType::kCannotImplicitlyCreateCollection) {
        return false;
    }

    try {
        cluster::createCollectionWithRouterLoop(opCtx, _nss);
        LOGV2_DEBUG(8037201, 3, "Successfully created collection", "nss"_attr = _nss);
    } catch (const DBException& ex) {
        LOGV2(8037200, "Could not create collection", "error"_attr = redact(ex.toStatus()));
        _lastError = boost::none;
        return false;
    }
    // Ensure the routing info is refreshed before the command is retried to avoid StaleConfig
    _lastError = LastErrorType::kStaleShardVersion;
    return true;
}

int CollectionRoutingInfoTargeter::getAproxNShardsOwningChunks() const {
    return mongo::getAproxNShardsOwningChunks(_cri);
}

bool CollectionRoutingInfoTargeter::isTargetedCollectionSharded() const {
    return _cri.isSharded();
}

bool CollectionRoutingInfoTargeter::isTrackedTimeSeriesBucketsNamespace() const {
    return mongo::isTrackedTimeSeriesBucketsNamespace(_cri);
}

bool CollectionRoutingInfoTargeter::isTrackedTimeSeriesNamespace() const {
    return mongo::isTrackedTimeSeriesNamespace(_cri);
}

bool CollectionRoutingInfoTargeter::timeseriesNamespaceNeedsRewrite(
    const NamespaceString& nss) const {
    return mongo::isTrackedTimeSeriesBucketsNamespace(_cri) && !nss.isTimeseriesBucketsCollection();
}

RoutingContext& CollectionRoutingInfoTargeter::getRoutingCtx() const {
    return *_routingCtx;
}

const CollectionRoutingInfo& CollectionRoutingInfoTargeter::getRoutingInfo() const {
    return _cri;
}

}  // namespace mongo
