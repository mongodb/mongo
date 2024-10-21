/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/commands/query_cmd/aggregation_execution_state.h"

namespace mongo {
StatusWith<StringMap<ResolvedNamespace>> AggExState::resolveInvolvedNamespaces() const {
    auto request = getRequest();
    auto pipelineInvolvedNamespaces = getInvolvedNamespaces();

    // If there are no involved namespaces, return before attempting to take any locks. This is
    // important for collectionless aggregations, which may be expected to run without locking.
    if (pipelineInvolvedNamespaces.empty()) {
        return {StringMap<ResolvedNamespace>()};
    }

    // Acquire a single const view of the CollectionCatalog and use it for all view and collection
    // lookups and view definition resolutions that follow. This prevents the view definitions
    // cached in 'resolvedNamespaces' from changing relative to those in the acquired ViewCatalog.
    // The resolution of the view definitions below might lead into an endless cycle if any are
    // allowed to change.
    auto catalog = CollectionCatalog::get(_opCtx);

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    StringMap<ResolvedNamespace> resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs.coll()) != resolvedNamespaces.end()) {
            continue;
        }

        // If 'ns' refers to a view namespace, then we resolve its definition.
        auto resolveViewDefinition = [&](const NamespaceString& ns) -> Status {
            auto resolvedView = view_catalog_helpers::resolveView(_opCtx, catalog, ns, boost::none);
            if (!resolvedView.isOK()) {
                return resolvedView.getStatus().withContext(str::stream()
                                                            << "Failed to resolve view '"
                                                            << involvedNs.toStringForErrorMsg());
            }

            auto&& underlyingNs = resolvedView.getValue().getNamespace();
            // Attempt to acquire UUID of the underlying collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(_opCtx, underlyingNs);
            resolvedNamespaces[ns.coll()] = {
                underlyingNs, resolvedView.getValue().getPipeline(), uuid};

            // We parse the pipeline corresponding to the resolved view in case we must resolve
            // other view namespaces that are also involved.
            LiteParsedPipeline resolvedViewLitePipeline(resolvedView.getValue().getNamespace(),
                                                        resolvedView.getValue().getPipeline());

            const auto& resolvedViewInvolvedNamespaces =
                resolvedViewLitePipeline.getInvolvedNamespaces();
            involvedNamespacesQueue.insert(involvedNamespacesQueue.end(),
                                           resolvedViewInvolvedNamespaces.begin(),
                                           resolvedViewInvolvedNamespaces.end());
            return Status::OK();
        };

        // If the involved namespace is not in the same database as the aggregation, it must be
        // from a $lookup/$graphLookup into a tenant migration donor's oplog view or from an
        // $out/$merge to a collection in a different database.
        if (!involvedNs.isEqualDb(request.getNamespace())) {
            if (involvedNs == NamespaceString::kTenantMigrationOplogView) {
                // For tenant migrations, we perform an aggregation on 'config.transactions' but
                // require a lookup stage involving a view on the 'local' database.
                // If the involved namespace is 'local.system.tenantMigration.oplogView', resolve
                // its view definition.
                auto status = resolveViewDefinition(involvedNs);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                // SERVER-51886: It is not correct to assume that we are reading from a collection
                // because the collection targeted by $out/$merge on a given database can have the
                // same name as a view on the source database. As such, we determine whether the
                // collection name references a view on the aggregation request's database. Note
                // that the inverse scenario (mistaking a view for a collection) is not an issue
                // because $merge/$out cannot target a view.
                auto nssToCheck = NamespaceStringUtil::deserialize(request.getNamespace().dbName(),
                                                                   involvedNs.coll());
                if (catalog->lookupView(_opCtx, nssToCheck)) {
                    auto status = resolveViewDefinition(nssToCheck);
                    if (!status.isOK()) {
                        return status;
                    }
                } else {
                    resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
                }
            }
        } else if (catalog->lookupCollectionByNamespace(_opCtx, involvedNs)) {
            // Attempt to acquire UUID of the collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(_opCtx, involvedNs);
            // If 'involvedNs' refers to a collection namespace, then we resolve it as an empty
            // pipeline in order to read directly from the underlying collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}, uuid};
        } else if (catalog->lookupView(_opCtx, involvedNs)) {
            auto status = resolveViewDefinition(involvedNs);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // 'involvedNs' is neither a view nor a collection, so resolve it as an empty pipeline
            // to treat it as reading from a non-existent collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        }
    }

    return resolvedNamespaces;
}

void AggExState::setView(std::shared_ptr<const CollectionCatalog> catalog,
                         const ViewDefinition* view) {
    // Queries on timeseries views may specify non-default collation whereas queries
    // on all other types of views must match the default collator (the collation used
    // to originally create that collections). Thus in the case of operations on TS
    // views, we use the request's collation.
    auto timeSeriesCollator =
        view->timeseries() ? _aggReqDerivatives->request.getCollation() : boost::none;

    auto resolvedView = uassertStatusOK(view_catalog_helpers::resolveView(
        _opCtx, catalog, _aggReqDerivatives->request.getNamespace(), timeSeriesCollator));

    uassert(std::move(resolvedView),
            "Explain of a resolved view must be executed by mongos",
            !ShardingState::get(_opCtx)->enabled() || !_aggReqDerivatives->request.getExplain());

    _resolvedView = resolvedView;

    // Parse the resolved view into a new AggregationRequestDerivatives object.
    _resolvedViewRequest = resolvedView.asExpandedViewAggregation(_aggReqDerivatives->request);
    _resolvedViewLiteParsedPipeline = _resolvedViewRequest.value();
    _aggReqDerivatives = std::make_unique<AggregateRequestDerivatives>(
        _resolvedViewRequest.value(), _resolvedViewLiteParsedPipeline.value(), [this]() {
            return aggregation_request_helper::serializeToCommandObj(_resolvedViewRequest.value());
        });

    _executionNss = _aggReqDerivatives->request.getNamespace();
}

void AggExState::performValidationChecks() {
    auto request = getRequest();
    auto& liteParsedPipeline = _aggReqDerivatives->liteParsedPipeline;

    liteParsedPipeline.validate(_opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(_opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);

    // If we are in a transaction, check whether the parsed pipeline supports being in
    // a transaction and if the transaction's read concern is supported.
    if (_opCtx->inMultiDocumentTransaction()) {
        liteParsedPipeline.assertSupportsMultiDocumentTransaction(request.getExplain());
        liteParsedPipeline.assertSupportsReadConcern(_opCtx, request.getExplain());
    }
}

ScopedSetShardRole AggExState::setShardRole(const CollectionRoutingInfo& cri) {
    const NamespaceString& underlyingNss = _resolvedView.value().getNamespace();

    const auto optPlacementConflictTimestamp = [&]() {
        auto originalShardVersion =
            OperationShardingState::get(_opCtx).getShardVersion(getOriginalNss());

        // Since for requests on timeseries namespaces the ServiceEntryPoint installs shard version
        // on the buckets collection instead of the viewNss.
        // TODO: SERVER-80719 Remove this.
        if (!originalShardVersion && underlyingNss.isTimeseriesBucketsCollection()) {
            originalShardVersion =
                OperationShardingState::get(_opCtx).getShardVersion(underlyingNss);
        }

        return originalShardVersion ? originalShardVersion->placementConflictTime() : boost::none;
    }();


    if (cri.cm.hasRoutingTable()) {
        const auto myShardId = ShardingState::get(_opCtx)->shardId();

        auto sv = cri.getShardVersion(myShardId);
        if (optPlacementConflictTimestamp) {
            sv.setPlacementConflictTime(*optPlacementConflictTimestamp);
        }
        return ScopedSetShardRole(
            _opCtx, underlyingNss, sv /*shardVersion*/, boost::none /*databaseVersion*/);
    } else {
        auto sv = ShardVersion::UNSHARDED();
        if (optPlacementConflictTimestamp) {
            sv.setPlacementConflictTime(*optPlacementConflictTimestamp);
        }
        return ScopedSetShardRole(_opCtx,
                                  underlyingNss,
                                  ShardVersion::UNSHARDED() /*shardVersion*/,
                                  cri.cm.dbVersion() /*databaseVersion*/);
    }
}

bool AggExState::canReadUnderlyingCollectionLocally(const CollectionRoutingInfo& cri) const {
    const auto myShardId = ShardingState::get(_opCtx)->shardId();
    const auto atClusterTime = repl::ReadConcernArgs::get(_opCtx).getArgsAtClusterTime();

    const auto chunkManagerMaybeAtClusterTime =
        atClusterTime ? ChunkManager::makeAtTime(cri.cm, atClusterTime->asTimestamp()) : cri.cm;

    if (chunkManagerMaybeAtClusterTime.isSharded()) {
        return false;
    } else if (chunkManagerMaybeAtClusterTime.isUnsplittable()) {
        return chunkManagerMaybeAtClusterTime.getMinKeyShardIdWithSimpleCollation() == myShardId;
    } else {
        return chunkManagerMaybeAtClusterTime.dbPrimary() == myShardId;
    }
}

Status AggExState::collatorCompatibleWithPipeline(const CollatorInterface* collator) const {
    auto catalog = CollectionCatalog::get(_opCtx);
    for (const auto& potentialViewNs : getInvolvedNamespaces()) {
        if (catalog->lookupCollectionByNamespace(_opCtx, potentialViewNs)) {
            continue;
        }

        auto view = catalog->lookupView(_opCtx, potentialViewNs);
        if (!view) {
            continue;
        }
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collator)) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "Cannot override a view's default collation"
                                  << potentialViewNs.toStringForErrorMsg()};
        }
    }
    return Status::OK();
}

void AggExState::adjustChangeStreamReadConcern() {
    repl::ReadConcernArgs& readConcernArgs = repl::ReadConcernArgs::get(_opCtx);
    // There is already a non-default read concern level set. Do nothing.
    if (readConcernArgs.hasLevel() && !readConcernArgs.getProvenance().isImplicitDefault()) {
        return;
    }
    // We upconvert an empty read concern to 'majority'.
    {
        // We must obtain the client lock to set the ReadConcernArgs on the operation
        // context as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    }

    // Wait for read concern again since we changed the original read concern.
    uassertStatusOK(waitForReadConcern(_opCtx, readConcernArgs, DatabaseName(), true));
    setPrepareConflictBehaviorForReadConcern(
        _opCtx, readConcernArgs, PrepareConflictBehavior::kIgnoreConflicts);
}
}  // namespace mongo
