/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_catalog.h"

#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
StatusWith<std::unique_ptr<CollatorInterface>> parseCollator(OperationContext* txn,
                                                             BSONObj collationSpec) {
    // If 'collationSpec' is empty, return the null collator, which represents the "simple"
    // collation.
    if (collationSpec.isEmpty()) {
        return {nullptr};
    }
    return CollatorFactoryInterface::get(txn->getServiceContext())->makeFromBSON(collationSpec);
}
}  // namespace

Status ViewCatalog::reloadIfNeeded(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _reloadIfNeeded_inlock(txn);
}

Status ViewCatalog::_reloadIfNeeded_inlock(OperationContext* txn) {
    if (_valid.load())
        return Status::OK();

    LOG(1) << "reloading view catalog for database " << _durable->getName();

    // Need to reload, first clear our cache.
    _viewMap.clear();

    Status status = _durable->iterate(txn, [&](const BSONObj& view) -> Status {
        BSONObj collationSpec = view.hasField("collation") ? view["collation"].Obj() : BSONObj();
        auto collator = parseCollator(txn, collationSpec);
        if (!collator.isOK()) {
            return collator.getStatus();
        }

        NamespaceString viewName(view["_id"].str());

        auto pipeline = view["pipeline"].Obj();
        for (auto&& stage : pipeline) {
            if (BSONType::Object != stage.type()) {
                return Status(ErrorCodes::InvalidViewDefinition,
                              str::stream() << "View 'pipeline' entries must be objects, but "
                                            << viewName.toString()
                                            << " has a pipeline element of type "
                                            << stage.type());
            }
        }

        _viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(viewName.db(),
                                                                   viewName.coll(),
                                                                   view["viewOn"].str(),
                                                                   pipeline,
                                                                   std::move(collator.getValue()));
        return Status::OK();
    });
    _valid.store(status.isOK());

    if (!status.isOK()) {
        LOG(0) << "could not load view catalog for database " << _durable->getName() << ": "
               << status;
    }

    return status;
}

void ViewCatalog::iterate(OperationContext* txn, ViewIteratorCallback callback) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _requireValidCatalog_inlock(txn);
    for (auto&& view : _viewMap) {
        callback(*view.second);
    }
}

Status ViewCatalog::_createOrUpdateView_inlock(OperationContext* txn,
                                               const NamespaceString& viewName,
                                               const NamespaceString& viewOn,
                                               const BSONArray& pipeline,
                                               std::unique_ptr<CollatorInterface> collator) {
    _requireValidCatalog_inlock(txn);

    // Build the BSON definition for this view to be saved in the durable view catalog. If the
    // collation is empty, omit it from the definition altogether.
    BSONObjBuilder viewDefBuilder;
    viewDefBuilder.append("_id", viewName.ns());
    viewDefBuilder.append("viewOn", viewOn.coll());
    viewDefBuilder.append("pipeline", pipeline);
    if (collator) {
        viewDefBuilder.append("collation", collator->getSpec().toBSON());
    }

    BSONObj ownedPipeline = pipeline.getOwned();
    auto view = std::make_shared<ViewDefinition>(
        viewName.db(), viewName.coll(), viewOn.coll(), ownedPipeline, std::move(collator));

    // Check that the resulting dependency graph is acyclic and within the maximum depth.
    Status graphStatus = _upsertIntoGraph(txn, *(view.get()));
    if (!graphStatus.isOK()) {
        return graphStatus;
    }

    _durable->upsert(txn, viewName, viewDefBuilder.obj());
    _viewMap[viewName.ns()] = view;
    txn->recoveryUnit()->onRollback([this, viewName]() {
        this->_viewMap.erase(viewName.ns());
        this->_viewGraphNeedsRefresh = true;
    });

    // We may get invalidated, but we're exclusively locked, so the change must be ours.
    txn->recoveryUnit()->onCommit([this]() { this->_valid.store(true); });
    return Status::OK();
}

Status ViewCatalog::_upsertIntoGraph(OperationContext* txn, const ViewDefinition& viewDef) {

    // Performs the insert into the graph.
    auto doInsert = [this, &txn](const ViewDefinition& viewDef, bool needsValidation) -> Status {
        // Make a LiteParsedPipeline to determine the namespaces referenced by this pipeline.
        AggregationRequest request(viewDef.viewOn(), viewDef.pipeline());
        const LiteParsedPipeline liteParsedPipeline(request);
        const auto involvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // Verify that this is a legitimate pipeline specification by making sure it parses
        // correctly. In order to parse a pipeline we need to resolve any namespaces involved to a
        // collection and a pipeline, but in this case we don't need this map to be accurate since
        // we will not be evaluating the pipeline.
        StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
        for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
            resolvedNamespaces[nss.coll()] = {nss, {}};
        }
        boost::intrusive_ptr<ExpressionContext> expCtx =
            new ExpressionContext(txn,
                                  request,
                                  CollatorInterface::cloneCollator(viewDef.defaultCollator()),
                                  std::move(resolvedNamespaces));
        auto pipelineStatus = Pipeline::parse(viewDef.pipeline(), expCtx);
        if (!pipelineStatus.isOK()) {
            uassert(40255,
                    str::stream() << "Invalid pipeline for view " << viewDef.name().ns() << "; "
                                  << pipelineStatus.getStatus().reason(),
                    !needsValidation);
            return pipelineStatus.getStatus();
        }

        std::vector<NamespaceString> refs(involvedNamespaces.begin(), involvedNamespaces.end());
        refs.push_back(viewDef.viewOn());

        int pipelineSize = 0;
        for (auto obj : viewDef.pipeline()) {
            pipelineSize += obj.objsize();
        }

        if (needsValidation) {
            // Check the collation of all the dependent namespaces before updating the graph.
            auto collationStatus = _validateCollation_inlock(txn, viewDef, refs);
            if (!collationStatus.isOK()) {
                return collationStatus;
            }
            return _viewGraph.insertAndValidate(viewDef, refs, pipelineSize);
        } else {
            _viewGraph.insertWithoutValidating(viewDef, refs, pipelineSize);
            return Status::OK();
        }
    };

    if (_viewGraphNeedsRefresh) {
        _viewGraph.clear();
        for (auto&& iter : _viewMap) {
            auto status = doInsert(*(iter.second.get()), false);
            // If we cannot fully refresh the graph, we will keep '_viewGraphNeedsRefresh' true.
            if (!status.isOK()) {
                return status;
            }
        }
        // Only if the inserts completed without error will we no longer need a refresh.
        txn->recoveryUnit()->onRollback([this]() { this->_viewGraphNeedsRefresh = true; });
        _viewGraphNeedsRefresh = false;
    }

    // Remove the view definition first in case this is an update. If it is not in the graph, it
    // is simply a no-op.
    _viewGraph.remove(viewDef.name());

    return doInsert(viewDef, true);
}

Status ViewCatalog::_validateCollation_inlock(OperationContext* txn,
                                              const ViewDefinition& view,
                                              const std::vector<NamespaceString>& refs) {
    for (auto&& potentialViewNss : refs) {
        auto otherView = _lookup_inlock(txn, potentialViewNss.ns());
        if (otherView &&
            !CollatorInterface::collatorsMatch(view.defaultCollator(),
                                               otherView->defaultCollator())) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "View " << view.name().toString()
                                  << " has conflicting collation with view "
                                  << otherView->name().toString()};
        }
    }
    return Status::OK();
}

Status ViewCatalog::createView(OperationContext* txn,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline,
                               const BSONObj& collation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k32 &&
        serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.load()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "Cannot create view when the featureCompatibilityVersion is 3.2. See "
                      "http://dochub.mongodb.org/core/3.4-feature-compatibility.");
    }

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (_lookup_inlock(txn, StringData(viewName.ns())))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    if (viewName.isSystem())
        return Status(
            ErrorCodes::InvalidNamespace,
            "View name cannot start with 'system.', which is reserved for system namespaces");

    auto collator = parseCollator(txn, collation);
    if (!collator.isOK())
        return collator.getStatus();

    return _createOrUpdateView_inlock(
        txn, viewName, viewOn, pipeline, std::move(collator.getValue()));
}

Status ViewCatalog::modifyView(OperationContext* txn,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k32 &&
        serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.load()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "Cannot modify view when the featureCompatibilityVersion is 3.2. See "
                      "http://dochub.mongodb.org/core/3.4-feature-compatibility.");
    }

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    auto viewPtr = _lookup_inlock(txn, viewName.ns());
    if (!viewPtr)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot modify missing view " << viewName.ns());

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    ViewDefinition savedDefinition = *viewPtr;
    txn->recoveryUnit()->onRollback([this, txn, viewName, savedDefinition]() {
        this->_viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(savedDefinition);
    });

    return _createOrUpdateView_inlock(
        txn,
        viewName,
        viewOn,
        pipeline,
        CollatorInterface::cloneCollator(savedDefinition.defaultCollator()));
}

Status ViewCatalog::dropView(OperationContext* txn, const NamespaceString& viewName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _requireValidCatalog_inlock(txn);

    // Save a copy of the view definition in case we need to roll back.
    auto viewPtr = _lookup_inlock(txn, viewName.ns());
    if (!viewPtr) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "cannot drop missing view: " << viewName.ns()};
    }

    ViewDefinition savedDefinition = *viewPtr;

    invariant(_valid.load());
    _durable->remove(txn, viewName);
    _viewGraph.remove(savedDefinition.name());
    _viewMap.erase(viewName.ns());
    txn->recoveryUnit()->onRollback([this, txn, viewName, savedDefinition]() {
        this->_viewGraphNeedsRefresh = true;
        this->_viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(savedDefinition);
    });

    // We may get invalidated, but we're exclusively locked, so the change must be ours.
    txn->recoveryUnit()->onCommit([this]() { this->_valid.store(true); });
    return Status::OK();
}

std::shared_ptr<ViewDefinition> ViewCatalog::_lookup_inlock(OperationContext* txn, StringData ns) {
    // We expect the catalog to be valid, so short-circuit other checks for best performance.
    if (MONGO_unlikely(!_valid.load())) {
        // If the catalog is invalid, we want to avoid references to virtualized or other invalid
        // collection names to trigger a reload. This makes the system more robust in presence of
        // invalid view definitions.
        if (!NamespaceString::validCollectionName(ns))
            return nullptr;
        Status status = _reloadIfNeeded_inlock(txn);
        // In case of errors we've already logged a message. Only uassert if there actually is
        // a user connection, as otherwise we'd crash the server. The catalog will remain invalid,
        // and any views after the first invalid one are ignored.
        if (txn->getClient()->isFromUserConnection())
            uassertStatusOK(status);
    }

    ViewMap::const_iterator it = _viewMap.find(ns);
    if (it != _viewMap.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<ViewDefinition> ViewCatalog::lookup(OperationContext* txn, StringData ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lookup_inlock(txn, ns);
}

StatusWith<ResolvedView> ViewCatalog::resolveView(OperationContext* txn,
                                                  const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    const NamespaceString* resolvedNss = &nss;
    std::vector<BSONObj> resolvedPipeline;

    for (int i = 0; i < ViewGraph::kMaxViewDepth; i++) {
        auto view = _lookup_inlock(txn, resolvedNss->ns());
        if (!view) {
            // Return error status if pipeline is too large.
            int pipelineSize = 0;
            for (auto obj : resolvedPipeline) {
                pipelineSize += obj.objsize();
            }
            if (pipelineSize > ViewGraph::kMaxViewPipelineSizeBytes) {
                return {ErrorCodes::ViewPipelineMaxSizeExceeded,
                        str::stream() << "View pipeline exceeds maximum size; maximum size is "
                                      << ViewGraph::kMaxViewPipelineSizeBytes};
            }
            return StatusWith<ResolvedView>({*resolvedNss, resolvedPipeline});
        }

        resolvedNss = &(view->viewOn());

        // Prepend the underlying view's pipeline to the current working pipeline.
        const std::vector<BSONObj>& toPrepend = view->pipeline();
        resolvedPipeline.insert(resolvedPipeline.begin(), toPrepend.begin(), toPrepend.end());

        // If the first stage is a $collStats, then we return early with the viewOn namespace.
        if (toPrepend.size() > 0 && !toPrepend[0]["$collStats"].eoo()) {
            return StatusWith<ResolvedView>({*resolvedNss, resolvedPipeline});
        }
    }

    return {ErrorCodes::ViewDepthLimitExceeded,
            str::stream() << "View depth too deep or view cycle detected; maximum depth is "
                          << ViewGraph::kMaxViewDepth};
}
}  // namespace mongo
