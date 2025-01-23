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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view_catalog_helpers.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterAcquiringCollectionCatalog);

/**
 * This class represents catalog state for normal (i.e., not change stream or collectionless)
 * pipelines.
 *
 * The catalog lock is managed in the RAII style, being acquired in the constructor and relinquished
 * in the destructor (unless relinquished early by calling 'relinquishLocks()').
 */
class DefaultAggCatalogState : public AggCatalogState {
public:
    /**
     * This class should never be copied or moved, since catalog locks are scoped to the lifetime of
     * exactly one instance of this class.
     */
    DefaultAggCatalogState(const DefaultAggCatalogState&) = delete;
    DefaultAggCatalogState(DefaultAggCatalogState&&) = delete;

    explicit DefaultAggCatalogState(const AggExState& aggExState)
        : AggCatalogState{aggExState},
          _secondaryExecNssList{aggExState.getForeignExecutionNamespaces()} {
        initContext(auto_get_collection::ViewMode::kViewsPermitted);
        if (_collections.hasMainCollection()) {
            _uuid = _collections.getMainCollection()->uuid();
        }
    }

    bool lockAcquired() const override {
        return _ctx.has_value();
    }

    std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
    resolveCollator() const override {
        return ::mongo::resolveCollator(
            _aggExState.getOpCtx(),
            _aggExState.getRequest().getCollation().get_value_or(BSONObj()),
            _collections.getMainCollection());
    }

    const CollectionPtr& getPrimaryCollection() const override {
        invariant(lockAcquired());
        return _ctx->getCollection();
    }

    query_shape::CollectionType getPrimaryCollectionType() const override {
        invariant(lockAcquired());
        return _ctx->getCollectionType();
    }

    const ViewDefinition* getPrimaryView() const override {
        invariant(lockAcquired());
        return _ctx->getView();
    }

    void getStatsTrackerIfNeeded(boost::optional<AutoStatsTracker>& statsTracker) const override {
        // By default '_ctx' will create a stats tracker, so no need to need to instantiate an
        // AutoStatsTracker here.
    }

    const MultipleCollectionAccessor& getCollections() const override {
        return _collections;
    }

    std::shared_ptr<const CollectionCatalog> getCatalog() const override {
        return _catalog;
    }

    boost::optional<UUID> getUUID() const override {
        return _uuid;
    }

    void relinquishLocks() override {
        _ctx.reset();
        _collections.clear();
    }

    ~DefaultAggCatalogState() override {}

private:
    /**
     * This is the method that does the actual acquisition and initialization of catalog data
     * structures, during construction of subclass instances.
     */
    void initContext(auto_get_collection::ViewMode viewMode) {
        auto initAutoGetCallback = [&]() {
            _ctx.emplace(_aggExState.getOpCtx(),
                         _aggExState.getExecutionNss(),
                         AutoGetCollection::Options{}.viewMode(viewMode).secondaryNssOrUUIDs(
                             _secondaryExecNssList.cbegin(), _secondaryExecNssList.cend()),
                         AutoStatsTracker::LogMode::kUpdateTopAndCurOp);
        };
        bool anySecondaryCollectionNotLocal = intializeAutoGet(_aggExState.getOpCtx(),
                                                               _aggExState.getExecutionNss(),
                                                               _secondaryExecNssList,
                                                               initAutoGetCallback);
        tassert(8322000,
                "Should have initialized AutoGet* after calling 'initializeAutoGet'",
                _ctx.has_value());
        _collections = MultipleCollectionAccessor(_aggExState.getOpCtx(),
                                                  &_ctx->getCollection(),
                                                  _ctx->getNss(),
                                                  _ctx->isAnySecondaryNamespaceAView() ||
                                                      anySecondaryCollectionNotLocal,
                                                  _secondaryExecNssList);

        // Return the catalog that gets implicitly stashed during the collection acquisition
        // above, which also implicitly opened a storage snapshot. This catalog object can
        // be potentially different than the one obtained before and will be in sync with
        // the opened snapshot.
        _catalog = CollectionCatalog::get(_aggExState.getOpCtx());

        hangAfterAcquiringCollectionCatalog.executeIf(
            [&](const auto&) { hangAfterAcquiringCollectionCatalog.pauseWhileSet(); },
            [&](const BSONObj& data) {
                return _aggExState.getExecutionNss().coll() == data["collection"].valueStringData();
            });
    }

protected:
    /**
     * Constructor used by Oplog subclass.
     */
    DefaultAggCatalogState(const AggExState& aggExState, auto_get_collection::ViewMode viewMode)
        : AggCatalogState{aggExState} {
        initContext(viewMode);
    }

    const std::vector<NamespaceStringOrUUID> _secondaryExecNssList;

    // If emplaced, AutoGetCollectionForReadCommandMaybeLockFree will throw if the sharding version
    // for this connection is out of date. If the namespace is a view, the lock will be released
    // before re-running the expanded aggregation.
    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> _ctx;
    MultipleCollectionAccessor _collections;
    std::shared_ptr<const CollectionCatalog> _catalog;
    boost::optional<UUID> _uuid;
};

/**
 * Change stream pipelines have some subtle differences from normal pipelines. This class
 * encapsulates them.
 */
class OplogAggCatalogState : public DefaultAggCatalogState {
public:
    explicit OplogAggCatalogState(const AggExState& aggExState)
        : DefaultAggCatalogState{aggExState, auto_get_collection::ViewMode::kViewsForbidden} {}

    void validate() const override {
        AggCatalogState::validate();

        // Raise an error if original nss is a view. We do not need to check this if we are opening
        // a stream on an entire db or across the cluster.
        if (!_aggExState.getOriginalNss().isCollectionlessAggregateNS()) {
            auto view = _catalog->lookupView(_aggExState.getOpCtx(), _aggExState.getOriginalNss());
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Cannot run aggregation on timeseries with namespace "
                                  << _aggExState.getOriginalNss().toStringForErrorMsg(),
                    !view || !view->timeseries());
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Namespace "
                                  << _aggExState.getOriginalNss().toStringForErrorMsg()
                                  << " is a view, not a collection",
                    !view);
        }
    }

    std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
    resolveCollator() const override {
        // If the user specified an explicit collation, adopt it; otherwise, use the simple
        // collation. We do not inherit the collection's default collation or UUID, since the stream
        // may be resuming from a point before the current UUID existed.
        return ::mongo::resolveCollator(
            _aggExState.getOpCtx(),
            _aggExState.getRequest().getCollation().get_value_or(BSONObj()),
            CollectionPtr{});
    }

    ~OplogAggCatalogState() override {}
};

/**
 * AggCatalogState subclass for pipelines that do not access any collections. They do not have a
 * main collection and they do not access other collections (e.g., via $lookup).
 */
class CollectionlessAggCatalogState : public AggCatalogState {
public:
    explicit CollectionlessAggCatalogState(const AggExState& aggExState)
        : AggCatalogState{aggExState}, _catalog(CollectionCatalog::latest(aggExState.getOpCtx())) {}

    bool lockAcquired() const override {
        // Collectionless pipelines never acquire catalog locks, and thus never contain a valid
        // catalog context.
        return false;
    }

    void getStatsTrackerIfNeeded(boost::optional<AutoStatsTracker>& tracker) const override {
        // If this is a collectionless aggregation, we won't create '_ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        tracker.emplace(_aggExState.getOpCtx(),
                        _aggExState.getExecutionNss(),
                        Top::LockType::NotLocked,
                        AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                        DatabaseProfileSettings::get(_aggExState.getOpCtx()->getServiceContext())
                            .getDatabaseProfileLevel(_aggExState.getExecutionNss().dbName()));
    }

    std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
    resolveCollator() const override {
        return ::mongo::resolveCollator(
            _aggExState.getOpCtx(),
            _aggExState.getRequest().getCollation().get_value_or(BSONObj()),
            CollectionPtr());
    }

    const CollectionPtr& getPrimaryCollection() const override {
        MONGO_UNREACHABLE;
    }

    query_shape::CollectionType getPrimaryCollectionType() const override {
        MONGO_UNREACHABLE;
    }

    const ViewDefinition* getPrimaryView() const override {
        MONGO_UNREACHABLE;
    }

    const MultipleCollectionAccessor& getCollections() const override {
        return _emptyMultipleCollectionAccessor;
    }

    std::shared_ptr<const CollectionCatalog> getCatalog() const override {
        return _catalog;
    }

    boost::optional<UUID> getUUID() const override {
        return boost::none;
    }

    void relinquishLocks() override {}

    ~CollectionlessAggCatalogState() override {}

private:
    static const MultipleCollectionAccessor _emptyMultipleCollectionAccessor;

    std::shared_ptr<const CollectionCatalog> _catalog;
};

const MultipleCollectionAccessor CollectionlessAggCatalogState::_emptyMultipleCollectionAccessor{};

}  // namespace

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
            resolvedNamespaces[ns.coll()] = {underlyingNs,
                                             resolvedView.getValue().getPipeline(),
                                             uuid,
                                             true /*involvedNamespaceIsAView*/};

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
        // from an $out/$merge to a collection in a different database.
        if (!involvedNs.isEqualDb(request.getNamespace())) {
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
            return _resolvedViewRequest.value().toBSON();
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
    const auto pipelineInvolvedNamespaces = getInvolvedNamespaces();
    if (pipelineInvolvedNamespaces.empty()) {
        return Status::OK();
    }

    auto catalog = CollectionCatalog::get(_opCtx);
    for (const auto& potentialViewNs : pipelineInvolvedNamespaces) {
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

std::unique_ptr<AggCatalogState> AggExState::createAggCatalogState() {
    std::unique_ptr<AggCatalogState> collectionState;
    if (hasChangeStream()) {
        // If this is a change stream, perform special checks and change the execution
        // namespace.
        uassert(4928900,
                str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                              << " is not supported for a change stream",
                !getRequest().getCollectionUUID());

        // Replace the execution namespace with the oplog.
        setExecutionNss(NamespaceString::kRsOplogNamespace);

        // In case of serverless the change stream will be opened on the change collection.
        const bool isServerless = change_stream_serverless_helpers::isServerlessEnvironment();
        if (isServerless) {
            const auto tenantId =
                change_stream_serverless_helpers::resolveTenantId(getOriginalNss().tenantId());

            uassert(
                ErrorCodes::BadValue, "Change streams cannot be used without tenant id", tenantId);
            setExecutionNss(NamespaceString::makeChangeCollectionNSS(tenantId));

            uassert(ErrorCodes::ChangeStreamNotEnabled,
                    "Change streams must be enabled before being used",
                    change_stream_serverless_helpers::isChangeStreamEnabled(
                        getOpCtx(), *getExecutionNss().tenantId()));
        }

        // Assert that a change stream on the config server is always opened on the oplog.
        tassert(6763400,
                str::stream() << "Change stream was unexpectedly opened on the namespace: "
                              << getExecutionNss().toStringForErrorMsg() << " in the config server",
                !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
                    getExecutionNss().isOplog());

        // Upgrade and wait for read concern if necessary.
        adjustChangeStreamReadConcern();

        collectionState = AggCatalogStateFactory::createOplogAggCatalogState(*this);
    } else if (getExecutionNss().isCollectionlessAggregateNS() && getInvolvedNamespaces().empty()) {
        // We get here for aggregations that are not against a specific collection, e.g.,
        //   { aggregate: 1, pipeline: [...] }
        // that also do not access any secondary collections (via $lookup for example).
        uassert(4928901,
                str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                              << " is not supported for a collectionless aggregation",
                !getRequest().getCollectionUUID());
        collectionState = AggCatalogStateFactory::createCollectionlessAggCatalogState(*this);
    } else {
        collectionState = AggCatalogStateFactory::createDefaultAggCatalogState(*this);
    }

    collectionState->validate();

    return collectionState;
}

boost::intrusive_ptr<ExpressionContext> AggCatalogState::createExpressionContext() {
    auto [collator, collationMatchesDefault] = resolveCollator();
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(_aggExState.getOpCtx(),
                                   _aggExState.getRequest(),
                                   allowDiskUseByDefault.load())
                      .collator(std::move(collator))
                      .collUUID(getUUID())
                      .mongoProcessInterface(MongoProcessInterface::create(_aggExState.getOpCtx()))
                      .mayDbProfile(CurOp::get(_aggExState.getOpCtx())->dbProfileLevel() > 0)
                      .resolvedNamespace(uassertStatusOK(_aggExState.resolveInvolvedNamespaces()))
                      .tmpDir(storageGlobalParams.dbpath + "/_tmp")
                      .collationMatchesDefault(collationMatchesDefault)
                      .build();
    // If any involved collection contains extended-range data, set a flag which individual
    // DocumentSource parsers can check.
    getCollections().forEach([&](const CollectionPtr& coll) {
        if (coll->getRequiresTimeseriesExtendedRangeSupport())
            expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    });

    return expCtx;
}

void AggCatalogState::validate() const {
    if (_aggExState.getRequest().getResumeAfter()) {
        uassert(ErrorCodes::InvalidPipelineOperator,
                "$_resumeAfter is not supported on view",
                !getPrimaryView());
        const auto& collection = getPrimaryCollection();
        const bool isClusteredCollection = collection && collection->isClustered();
        uassertStatusOK(query_request_helper::validateResumeInput(
            _aggExState.getOpCtx(),
            _aggExState.getRequest().getResumeAfter() ? *_aggExState.getRequest().getResumeAfter()
                                                      : BSONObj(),
            _aggExState.getRequest().getStartAt() ? *_aggExState.getRequest().getStartAt()
                                                  : BSONObj(),
            isClusteredCollection));
    }

    // If collectionUUID was provided, verify the collection exists and has the expected UUID.
    checkCollectionUUIDMismatch(_aggExState.getOpCtx(),
                                _aggExState.getExecutionNss(),
                                getCollections().getMainCollection(),
                                _aggExState.getRequest().getCollectionUUID());
}

/**
 * Determines the collection type of the query by precedence of various configurations. The order of
 * these checks is critical since there may be overlap (e.g., a view over a virtual collection is
 * classified as a view).
 */
query_shape::CollectionType AggCatalogState::determineCollectionType() const {
    if (_aggExState.getResolvedView().has_value()) {
        if (_aggExState.getResolvedView()->timeseries()) {
            return query_shape::CollectionType::kTimeseries;
        }
        return query_shape::CollectionType::kView;
    }

    if (_aggExState.getExecutionNss().isCollectionlessAggregateNS()) {
        // Note that the notion of "collectionless" here is different from how the word is used for
        // CollectionlessAggCatalogState. In this case we only care if the pipeline lacks a main
        // collection, e.g., '{ aggregate: 1, pipeline: [...] }'. It might be accessing secondary
        // collections via $lookup, but we still consider it collectionless for purposes of
        // determining the collection type.
        return query_shape::CollectionType::kVirtual;
    }

    if (_aggExState.hasChangeStream()) {
        return query_shape::CollectionType::kChangeStream;
    }
    return lockAcquired() ? getPrimaryCollectionType() : query_shape::CollectionType::kUnknown;
}

std::unique_ptr<AggCatalogState> AggCatalogStateFactory::createDefaultAggCatalogState(
    const AggExState& aggExState) {
    return std::make_unique<DefaultAggCatalogState>(aggExState);
}

std::unique_ptr<AggCatalogState> AggCatalogStateFactory::createOplogAggCatalogState(
    const AggExState& aggExState) {
    return std::make_unique<OplogAggCatalogState>(aggExState);
}

std::unique_ptr<AggCatalogState> AggCatalogStateFactory::createCollectionlessAggCatalogState(
    const AggExState& aggExState) {
    auto cs = std::make_unique<CollectionlessAggCatalogState>(aggExState);
    tassert(6235101, "A collection-less aggregate should not take any locks", !cs->lockAcquired());
    return cs;
}

}  // namespace mongo
