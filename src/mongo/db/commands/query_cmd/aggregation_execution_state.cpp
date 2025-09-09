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

#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/view_catalog_helpers.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterAcquiringCollectionCatalog);

/**
 * This class represents catalog state for normal (i.e., not change stream or collectionless)
 * pipelines.
 *
 * The catalog resources are managed in the RAII style, being acquired in the constructor and
 * relinquished in the destructor (unless relinquished early by calling 'relinquishResources()').
 */
class DefaultAggCatalogState : public AggCatalogState {
public:
    DefaultAggCatalogState(const DefaultAggCatalogState&) = delete;
    DefaultAggCatalogState(DefaultAggCatalogState&&) = delete;

    explicit DefaultAggCatalogState(const AggExState& aggExState)
        : AggCatalogState{aggExState},
          _secondaryExecNssList{aggExState.getForeignExecutionNamespaces()} {
        initContext(AcquisitionPrerequisites::ViewMode::kCanBeView);
        if (_collections.hasMainCollection()) {
            _uuid = _collections.getMainCollection()->uuid();
        }
    }

    bool lockAcquired() const override {
        return _mainAcq.has_value();
    }

    std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
    resolveCollator() const override {
        return ::mongo::resolveCollator(
            _aggExState.getOpCtx(),
            _aggExState.getRequest().getCollation().get_value_or(BSONObj()),
            _collections.getMainCollection());
    }

    const CollectionOrViewAcquisition& getMainCollectionOrView() const override {
        tassert(10240801, "Expected resources to be acquired", lockAcquired());
        return *_mainAcq;
    }

    query_shape::CollectionType getMainCollectionType() const override {
        tassert(10240802, "Expected resources to be acquired", lockAcquired());
        return _mainAcq->getCollectionType();
    }

    void getStatsTrackerIfNeeded(boost::optional<AutoStatsTracker>& statsTracker) const override {
        // By default '_mainAcq' will create a stats tracker, so no need to need to instantiate an
        // AutoStatsTracker here.
    }

    const MultipleCollectionAccessor& getCollections() const override {
        return _collections;
    }

    std::shared_ptr<const CollectionCatalog> getCatalog() const override {
        return _catalog;
    }

    StatusWith<ResolvedView> resolveView(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<BSONObj> timeSeriesCollator) const override {
        return view_catalog_helpers::resolveView(opCtx, _catalog, nss, timeSeriesCollator);
    }

    boost::optional<UUID> getUUID() const override {
        return _uuid;
    }

    /**
     * Returns true if the collection is a viewful or viewless timeseries collection.
     */
    bool isTimeseries() const override {
        if (!lockAcquired()) {
            return false;
        }
        if (_mainAcq->isView() && _mainAcq->getView().getViewDefinition().timeseries()) {
            return true;
        }
        if (_mainAcq->collectionExists() &&
            _mainAcq->getCollectionPtr()->isTimeseriesCollection()) {
            return true;
        }
        return false;
    }

    void relinquishResources() override {
        _mainAcq.reset();
        _collections.clear();
    }

    void stashResources(TransactionResourcesStasher* transactionResourcesStasher) override {
        tassert(10096103,
                "DefaultAggCatalogStateWithAcquisition::stashResources got null stasher",
                transactionResourcesStasher);

        // First release our own CollectionAcquisitions references.
        relinquishResources();

        // Stash the remaining ShardRole::TransactionResources that back the CollectionAcquisitions
        // currently held by the query plan executor.
        stashTransactionResourcesFromOperationContext(_aggExState.getOpCtx(),
                                                      transactionResourcesStasher);
    }

private:
    /**
     * This is the method that does the actual acquisition and initialization of catalog data
     * structures, during construction of subclass instances.
     */
    void initContext(const AcquisitionPrerequisites::ViewMode& viewMode) {
        auto opCtx = _aggExState.getOpCtx();
        auto executionNss = _aggExState.getExecutionNss();

        AutoStatsTracker statsTracker(
            opCtx,
            executionNss,
            Top::LockType::ReadLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            DatabaseProfileSettings::get(_aggExState.getOpCtx()->getServiceContext())
                .getDatabaseProfileLevel(_aggExState.getExecutionNss().dbName()),
            Date_t::max(),
            _secondaryExecNssList.begin(),
            _secondaryExecNssList.end());

        CollectionOrViewAcquisitionMap secondaryAcquisitions;
        secondaryAcquisitions.reserve(_secondaryExecNssList.size() + 1);
        auto initAutoGetCallback = [&]() {
            CollectionOrViewAcquisitionRequests acquisitionRequests;
            acquisitionRequests.reserve(_secondaryExecNssList.size() + 1);

            // Emplace the main acquisition request.
            acquisitionRequests.emplace_back(CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, executionNss, AcquisitionPrerequisites::kRead, viewMode));

            // Emplace a request for every secondary nss.
            for (auto& nss : _secondaryExecNssList) {
                auto r = CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nss, AcquisitionPrerequisites::kRead, viewMode);
                acquisitionRequests.emplace_back(std::move(r));
            }

            // Acquire all the collection and extract the main acquisition
            secondaryAcquisitions = makeAcquisitionMap(
                acquireCollectionsOrViewsMaybeLockFree(opCtx, acquisitionRequests));
            _mainAcq.emplace(secondaryAcquisitions.extract(executionNss).mapped());
        };

        bool isAnySecondaryCollectionNotLocal = initializeAutoGet(_aggExState.getOpCtx(),
                                                                  _aggExState.getExecutionNss(),
                                                                  _secondaryExecNssList,
                                                                  initAutoGetCallback);

        uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
            opCtx,
            _aggExState.getExecutionNss(),
            ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        bool isAnySecondaryNamespaceAView =
            std::any_of(secondaryAcquisitions.begin(),
                        secondaryAcquisitions.end(),
                        [](const auto& acq) { return acq.second.isView(); });

        bool isAnySecondaryNamespaceAViewOrNotFullyLocal =
            isAnySecondaryNamespaceAView || isAnySecondaryCollectionNotLocal;

        _collections = MultipleCollectionAccessor(
            *_mainAcq, secondaryAcquisitions, isAnySecondaryNamespaceAViewOrNotFullyLocal);
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
    DefaultAggCatalogState(const AggExState& aggExState,
                           const AcquisitionPrerequisites::ViewMode& viewMode)
        : AggCatalogState{aggExState} {
        initContext(viewMode);
    }

    const std::vector<NamespaceStringOrUUID> _secondaryExecNssList;

    // If the namespace is a view, the lock will be released before re-running the expanded
    // aggregation.
    boost::optional<CollectionOrViewAcquisition> _mainAcq;
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
        : DefaultAggCatalogState{aggExState,
                                 AcquisitionPrerequisites::ViewMode::kMustBeCollection} {}

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
        // collation. We do not inherit the collection's default collation or UUID, since the
        // stream may be resuming from a point before the current UUID existed.
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
        // If this is a collectionless aggregation, we won't create '_ctx' but will still need
        // an AutoStatsTracker to record CurOp and Top entries.
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

    const CollectionOrViewAcquisition& getMainCollectionOrView() const override {
        MONGO_UNREACHABLE;
    }

    query_shape::CollectionType getMainCollectionType() const override {
        MONGO_UNREACHABLE;
    }

    const MultipleCollectionAccessor& getCollections() const override {
        return _emptyMultipleCollectionAccessor;
    }

    std::shared_ptr<const CollectionCatalog> getCatalog() const override {
        return _catalog;
    }

    StatusWith<ResolvedView> resolveView(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<BSONObj> timeSeriesCollator) const override {
        return view_catalog_helpers::resolveView(opCtx, _catalog, nss, timeSeriesCollator);
    }

    boost::optional<UUID> getUUID() const override {
        return boost::none;
    }

    bool isTimeseries() const final {
        return false;
    }

    void relinquishResources() override {}

    void stashResources(TransactionResourcesStasher* transactionResourcesStasher) override {}

    ~CollectionlessAggCatalogState() override {}

private:
    static const MultipleCollectionAccessor _emptyMultipleCollectionAccessor;

    std::shared_ptr<const CollectionCatalog> _catalog;
};

const MultipleCollectionAccessor CollectionlessAggCatalogState::_emptyMultipleCollectionAccessor{};

}  // namespace

StatusWith<ResolvedNamespaceMap> AggExState::resolveInvolvedNamespaces() const {
    auto request = getRequest();
    auto pipelineInvolvedNamespaces = getInvolvedNamespaces();

    // If there are no involved namespaces, return before attempting to take any locks. This is
    // important for collectionless aggregations, which may be expected to run without locking.
    if (pipelineInvolvedNamespaces.empty()) {
        return {ResolvedNamespaceMap()};
    }

    // Acquire a single const view of the CollectionCatalog and use it for all view and collection
    // lookups and view definition resolutions that follow. This prevents the view definitions
    // cached in 'resolvedNamespaces' from changing relative to those in the acquired ViewCatalog.
    // The resolution of the view definitions below might lead into an endless cycle if any are
    // allowed to change.
    auto catalog = CollectionCatalog::get(_opCtx);

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    ResolvedNamespaceMap resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs) != resolvedNamespaces.end()) {
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
            resolvedNamespaces[ns] = {underlyingNs,
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
                resolvedNamespaces[involvedNs] = {involvedNs, std::vector<BSONObj>{}};
            }
        } else if (catalog->lookupCollectionByNamespace(_opCtx, involvedNs)) {
            // Attempt to acquire UUID of the collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(_opCtx, involvedNs);
            // If 'involvedNs' refers to a collection namespace, then we resolve it as an empty
            // pipeline in order to read directly from the underlying collection.
            resolvedNamespaces[involvedNs] = {involvedNs, std::vector<BSONObj>{}, uuid};
        } else if (catalog->lookupView(_opCtx, involvedNs)) {
            auto status = resolveViewDefinition(involvedNs);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // 'involvedNs' is neither a view nor a collection, so resolve it as an empty pipeline
            // to treat it as reading from a non-existent collection.
            resolvedNamespaces[involvedNs] = {involvedNs, std::vector<BSONObj>{}};
        }
    }

    return resolvedNamespaces;
}

void AggExState::performValidationChecks() {
    auto request = getRequest();
    auto& liteParsedPipeline = _aggReqDerivatives->liteParsedPipeline;

    liteParsedPipeline.validate(_opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(_opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);

    // If we are in a transaction, check whether the parsed pipeline supports being in
    // a transaction and if the transaction's read concern is supported.
    bool isExplain = request.getExplain().get_value_or(false);
    if (_opCtx->inMultiDocumentTransaction()) {
        liteParsedPipeline.assertSupportsMultiDocumentTransaction(isExplain);
        liteParsedPipeline.assertSupportsReadConcern(_opCtx, isExplain);
    }
}

bool AggExState::canReadUnderlyingCollectionLocally(const CollectionRoutingInfo& cri) const {
    const auto myShardId = ShardingState::get(_opCtx)->shardId();
    const auto atClusterTime = repl::ReadConcernArgs::get(_opCtx).getArgsAtClusterTime();

    const auto chunkManagerMaybeAtClusterTime = atClusterTime
        ? ChunkManager::makeAtTime(cri.getChunkManager(), atClusterTime->asTimestamp())
        : cri.getChunkManager();

    if (chunkManagerMaybeAtClusterTime.isSharded()) {
        return false;
    } else if (chunkManagerMaybeAtClusterTime.isUnsplittable()) {
        return chunkManagerMaybeAtClusterTime.getMinKeyShardIdWithSimpleCollation() == myShardId;
    } else {
        return cri.getDbPrimaryShardId() == myShardId;
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
            const auto tenantId = change_stream_serverless_helpers::resolveTenantId(
                VersionContext::getDecoration(getOpCtx()), getOriginalNss().tenantId());

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

        if (isRawDataOperation(getOpCtx())) {
            auto [isTimeseriesViewRequest, translatedNs] =
                timeseries::isTimeseriesViewRequest(getOpCtx(), getRequest());
            if (isTimeseriesViewRequest) {
                setExecutionNss(translatedNs);
                collectionState->relinquishResources();
                collectionState = AggCatalogStateFactory::createDefaultAggCatalogState(*this);
            }
        }
    }

    collectionState->validate();

    return collectionState;
}

ResolvedViewAggExState::ResolvedViewAggExState(AggExState&& baseState,
                                               std::unique_ptr<AggCatalogState>& aggCatalogStage,
                                               const ViewDefinition& view)
    : AggExState(std::move(baseState)),
      _originalAggReqDerivatives(std::move(_aggReqDerivatives)),
      _resolvedView(uassertStatusOK(aggCatalogStage->resolveView(
          _opCtx,
          _originalAggReqDerivatives->request.getNamespace(),
          view.timeseries() ? _originalAggReqDerivatives->request.getCollation() : boost::none))),
      _resolvedViewRequest_DO_NOT_USE_DIRECTLY(PipelineResolver::buildRequestWithResolvedPipeline(
          _resolvedView, _originalAggReqDerivatives->request)),
      _resolvedViewLiteParsedPipeline_DO_NOT_USE_DIRECTLY(_resolvedViewRequest_DO_NOT_USE_DIRECTLY,
                                                          true) {
    bool isExplain = _originalAggReqDerivatives->request.getExplain().get_value_or(false);
    uassert(std::move(_resolvedView),
            "Explain of a resolved view must be executed by mongos",
            !ShardingState::get(_opCtx)->enabled() || !isExplain);

    // Parse the resolved view into a new AggregationRequestDerivatives object.
    _aggReqDerivatives = std::make_unique<AggregateRequestDerivatives>(
        _resolvedViewRequest_DO_NOT_USE_DIRECTLY,
        _resolvedViewLiteParsedPipeline_DO_NOT_USE_DIRECTLY);

    setExecutionNss(_resolvedView.getNamespace());
}

StatusWith<std::unique_ptr<ResolvedViewAggExState>> ResolvedViewAggExState::create(
    AggExState&& aggExState, std::unique_ptr<AggCatalogState>& aggCatalogState) {
    invariant(aggCatalogState->lockAcquired());

    // Resolve the request's collation and check that the default collation of 'view' is compatible
    // with the operation's collation. The collation resolution and check are both skipped if the
    // request did not specify a collation.
    tassert(10240800, "Expected a view", aggCatalogState->getMainCollectionOrView().isView());
    const auto& viewDefinition =
        aggCatalogState->getMainCollectionOrView().getView().getViewDefinition();

    if (!aggExState.getRequest().getCollation().get_value_or(BSONObj()).isEmpty()) {
        auto [collatorToUse, collatorToUseMatchesDefault] = aggCatalogState->resolveCollator();
        if (!CollatorInterface::collatorsMatch(viewDefinition.defaultCollator(),
                                               collatorToUse.get()) &&
            !viewDefinition.timeseries()) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    "Cannot override a view's default collation"};
        }
    }

    // Create the ResolvedViewAggExState object which will resolve the view upon
    // initialization.
    return std::make_unique<ResolvedViewAggExState>(
        std::move(aggExState), aggCatalogState, viewDefinition);
}

std::unique_ptr<Pipeline> ResolvedViewAggExState::applyViewToPipeline(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<UUID> uuid) const {
    if (getResolvedView().timeseries()) {
        // For timeseries, there may have been rewrites done on the raw BSON pipeline
        // during view resolution. We must parse the request's full resolved pipeline
        // which will account for those rewrites.
        // TODO SERVER-101599 remove this code once 9.0 becomes last LTS. By then only viewless
        // timeseries collections will exist.
        return Pipeline::parse(getRequest().getPipeline(), expCtx);
    } else if (search_helpers::isMongotPipeline(pipeline.get())) {
        // For search queries on views don't do any of the pipeline stitching that is done for
        // normal views.
        return pipeline;
    }

    // Parse the view pipeline, then stitch the user pipeline and view pipeline together
    // to build the total aggregation pipeline.
    auto userPipeline = std::move(pipeline);
    pipeline = Pipeline::parse(getResolvedView().getPipeline(), expCtx);
    pipeline->appendPipeline(std::move(userPipeline));
    return pipeline;
}

ScopedSetShardRole ResolvedViewAggExState::setShardRole(const CollectionRoutingInfo& cri) {
    const NamespaceString& underlyingNss = getExecutionNss();

    const auto optPlacementConflictTimestamp = [&]() {
        auto originalShardVersion =
            OperationShardingState::get(_opCtx).getShardVersion(getOriginalNss());

        // Since for requests on timeseries namespaces the ServiceEntryPoint installs shard
        // version
        // on the buckets collection instead of the viewNss.
        // TODO: SERVER-80719 Remove this.
        if (!originalShardVersion && underlyingNss.isTimeseriesBucketsCollection()) {
            originalShardVersion =
                OperationShardingState::get(_opCtx).getShardVersion(underlyingNss);
        }

        return originalShardVersion ? originalShardVersion->placementConflictTime() : boost::none;
    }();

    if (cri.hasRoutingTable()) {
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
        return ScopedSetShardRole(
            _opCtx, underlyingNss, sv /*shardVersion*/, cri.getDbVersion() /*databaseVersion*/);
    }
}

bool AggCatalogState::requiresExtendedRangeSupportForTimeseries(
    const ResolvedNamespaceMap& resolvedNamespaces) const {
    auto requiresExtendedRange = false;

    // Check the in-memory collections.
    getCollections().forEach([&](const CollectionPtr& coll) {
        if (coll->getRequiresTimeseriesExtendedRangeSupport()) {
            requiresExtendedRange = true;
        }
    });

    // It's possible that an involved nss that resolves to a timeseries buckets collection requires
    // extended range support (e.g. in the foreign coll of a $lookup), so we check for that as well.
    if (!requiresExtendedRange) {
        for (auto& [_, resolvedNs] : resolvedNamespaces) {
            const auto& nss = resolvedNs.ns;
            if (nss.isTimeseriesBucketsCollection()) {
                auto readTimestamp = shard_role_details::getRecoveryUnit(_aggExState.getOpCtx())
                                         ->getPointInTimeReadTimestamp();
                auto collPtr = CollectionPtr(getCatalog()->establishConsistentCollection(
                    _aggExState.getOpCtx(), NamespaceStringOrUUID(nss), readTimestamp));
                if (collPtr && collPtr->getRequiresTimeseriesExtendedRangeSupport()) {
                    requiresExtendedRange = true;
                    break;
                }
            }
        }
    }

    return requiresExtendedRange;
}

boost::intrusive_ptr<ExpressionContext> AggCatalogState::createExpressionContext() {
    auto [collator, collationMatchesDefault] = resolveCollator();
    const bool canPipelineBeRejected =
        query_settings::canPipelineBeRejected(_aggExState.getRequest().getPipeline());

    // If any involved collection contains extended-range data, set a flag which individual
    // DocumentSource parsers can check.
    const auto& resolvedNamespaces = uassertStatusOK(_aggExState.resolveInvolvedNamespaces());
    auto requiresExtendedRange = requiresExtendedRangeSupportForTimeseries(resolvedNamespaces);

    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(_aggExState.getOpCtx(),
                                   _aggExState.getRequest(),
                                   allowDiskUseByDefault.load())
                      .collator(std::move(collator))
                      .collUUID(getUUID())
                      .mongoProcessInterface(MongoProcessInterface::create(_aggExState.getOpCtx()))
                      .mayDbProfile(CurOp::get(_aggExState.getOpCtx())->dbProfileLevel() > 0)
                      .ns(_aggExState.hasChangeStream() ? _aggExState.getOriginalNss()
                                                        : _aggExState.getExecutionNss())
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .originalNs(_aggExState.getOriginalNss())
                      .requiresTimeseriesExtendedRangeSupport(requiresExtendedRange)
                      .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                      .collationMatchesDefault(collationMatchesDefault)
                      .canBeRejected(canPipelineBeRejected)
                      .explain(_aggExState.getVerbosity())
                      .build();

    // If the pipeline contains $exchange, set a flag so the individual
    // DocumentSources can check. Pipelines containing $exchange are incompatible with reporting
    // memory tracking to upstream channels like CurOp because they create multiple cursors.
    if (_aggExState.getRequest().getExchange().has_value()) {
        expCtx->setIncompatibleWithMemoryTracking(true);
    }

    return expCtx;
}

void AggCatalogState::validate() const {
    if (_aggExState.getRequest().getResumeAfter() || _aggExState.getRequest().getStartAt()) {
        const auto& collectionOrView = getMainCollectionOrView();
        uassert(ErrorCodes::InvalidPipelineOperator,
                "$_resumeAfter is not supported on view",
                !collectionOrView.isView());
        const bool isClusteredCollection = collectionOrView.collectionExists() &&
            collectionOrView.getCollection().getCollectionPtr()->isClustered();
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
    if (isTimeseries()) {
        return query_shape::CollectionType::kTimeseries;
    }

    if (_aggExState.isView()) {
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
    return lockAcquired() ? getMainCollectionType() : query_shape::CollectionType::kUnknown;
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
