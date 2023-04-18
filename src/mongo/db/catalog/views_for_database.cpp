/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "views_for_database.h"

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/views/util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
RecordId find(OperationContext* opCtx,
              const CollectionPtr& systemViews,
              const NamespaceString& viewName) {
    return systemViews->getIndexCatalog()
        ->findIdIndex(opCtx)
        ->getEntry()
        ->accessMethod()
        ->asSortedData()
        ->findSingle(opCtx, systemViews, BSON("_id" << NamespaceStringUtil::serialize(viewName)));
}

StatusWith<std::unique_ptr<CollatorInterface>> parseCollator(OperationContext* opCtx,
                                                             const BSONObj& collator) {
    // If 'collationSpec' is empty, return the null collator, which represents the "simple"
    // collation.
    return !collator.isEmpty()
        ? CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collator)
        : nullptr;
}
}  // namespace

std::shared_ptr<const ViewDefinition> ViewsForDatabase::lookup(const NamespaceString& ns) const {
    auto it = _viewMap.find(ns.coll());
    return it != _viewMap.end() ? it->second : nullptr;
}

void ViewsForDatabase::iterate(
    const std::function<bool(const ViewDefinition& view)>& callback) const {
    for (auto&& view : _viewMap) {
        if (!callback(*view.second)) {
            return;
        }
    }
}

Status ViewsForDatabase::reload(OperationContext* opCtx, const CollectionPtr& systemViews) {
    _viewMap.clear();
    _valid = false;
    _viewGraphNeedsRefresh = true;
    _stats = {};

    if (!systemViews) {
        _valid = true;
        return Status::OK();
    }

    invariant(opCtx->lockState()->isCollectionLockedForMode(systemViews->ns(), MODE_IS));

    auto cursor = systemViews->getCursor(opCtx);
    while (auto record = cursor->next()) {
        // Check the document is valid BSON, with only the expected fields.
        // Use the latest BSON validation version. Existing view definitions are allowed to contain
        // decimal data even if decimal is disabled.
        fassert(40224, validateBSON(record->data.data(), record->data.size()));

        auto view = record->data.toBson();
        try {
            view_util::validateViewDefinitionBSON(opCtx, view, systemViews->ns().dbName());
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        auto collatorElem = view["collation"];
        auto collator = parseCollator(opCtx, collatorElem ? collatorElem.Obj() : BSONObj{});
        if (!collator.isOK()) {
            return collator.getStatus();
        }

        auto viewName = NamespaceStringUtil::deserialize(systemViews->ns().tenantId(),
                                                         view.getStringField("_id"));

        if (auto status = _upsertIntoMap(
                opCtx,
                std::make_shared<ViewDefinition>(viewName.dbName(),
                                                 viewName.coll(),
                                                 view.getStringField("viewOn"),
                                                 BSONArray{view.getObjectField("pipeline")},
                                                 std::move(collator.getValue())));
            !status.isOK()) {
            LOGV2(22547,
                  "Could not load view catalog for database",
                  logAttrs(systemViews->ns().dbName()),
                  "error"_attr = status);

            return status;
        }
    }

    _valid = true;
    return Status::OK();
}

Status ViewsForDatabase::insert(OperationContext* opCtx,
                                const CollectionPtr& systemViews,
                                const NamespaceString& viewName,
                                const NamespaceString& viewOn,
                                const BSONArray& pipeline,
                                const PipelineValidatorFn& validatePipeline,
                                const BSONObj& collator,
                                Durability durability) {
    _valid = false;

    auto parsedCollator = parseCollator(opCtx, collator);
    if (!parsedCollator.isOK()) {
        return parsedCollator.getStatus();
    }

    auto view = std::make_shared<ViewDefinition>(viewName.dbName(),
                                                 viewName.coll(),
                                                 viewOn.coll(),
                                                 pipeline,
                                                 std::move(parsedCollator.getValue()));

    // Skip validating the view graph if the view is already durable.
    if (auto status = _upsertIntoGraph(
            opCtx, *view, validatePipeline, durability == Durability::kNotYetDurable);
        !status.isOK()) {
        return status;
    }

    if (durability == Durability::kNotYetDurable) {
        if (auto status = _upsertIntoCatalog(opCtx, systemViews, *view); !status.isOK()) {
            return status;
        }
    }

    if (auto status = _upsertIntoMap(opCtx, std::move(view)); !status.isOK()) {
        LOGV2(5387000, "Could not insert view", logAttrs(viewName.dbName()), "error"_attr = status);
        return status;
    }

    _valid = true;
    return Status::OK();
};

Status ViewsForDatabase::update(OperationContext* opCtx,
                                const CollectionPtr& systemViews,
                                const NamespaceString& viewName,
                                const NamespaceString& viewOn,
                                const BSONArray& pipeline,
                                const PipelineValidatorFn& validatePipeline,
                                std::unique_ptr<CollatorInterface> collator) {
    _valid = false;

    auto view = std::make_shared<ViewDefinition>(
        viewName.dbName(), viewName.coll(), viewOn.coll(), pipeline, std::move(collator));

    if (auto status = _upsertIntoGraph(opCtx, *view, validatePipeline, true); !status.isOK()) {
        return status;
    }

    if (auto status = _upsertIntoCatalog(opCtx, systemViews, *view); !status.isOK()) {
        return status;
    }

    if (auto status = reload(opCtx, systemViews); !status.isOK()) {
        return status;
    }

    _valid = true;
    return Status::OK();
}

Status ViewsForDatabase::_upsertIntoMap(OperationContext* opCtx,
                                        std::shared_ptr<ViewDefinition> view) {
    // Cannot have a secondary view on a system.buckets collection, only the time-series
    // collection view.
    if (view->viewOn().isTimeseriesBucketsCollection() &&
        view->name() != view->viewOn().getTimeseriesViewNamespace()) {
        return {
            ErrorCodes::InvalidNamespace,
            "Invalid view: cannot define a view over a system.buckets namespace except by "
            "creating a time-series collection",
        };
    }

    if (!view->name().isOnInternalDb() && !view->name().isSystem()) {
        if (view->timeseries()) {
            _stats.userTimeseries += 1;
        } else {
            _stats.userViews += 1;
        }
    } else {
        _stats.internal += 1;
    }

    _viewMap[view->name().coll()] = view;
    return Status::OK();
}

Status ViewsForDatabase::_upsertIntoGraph(OperationContext* opCtx,
                                          const ViewDefinition& viewDef,
                                          const PipelineValidatorFn& validatePipeline,
                                          bool needsValidation) {
    // Performs the insert into the graph.
    auto doInsert = [this, opCtx, &validatePipeline](const ViewDefinition& viewDef,
                                                     bool needsValidation) -> Status {
        // Validate that the pipeline is eligible to serve as a view definition. If it is, this
        // will also return the set of involved namespaces.
        auto pipelineStatus = validatePipeline(opCtx, viewDef);
        if (!pipelineStatus.isOK()) {
            if (needsValidation) {
                uassertStatusOKWithContext(pipelineStatus.getStatus(),
                                           str::stream() << "Invalid pipeline for view "
                                                         << viewDef.name().toStringForErrorMsg());
            }
            return pipelineStatus.getStatus();
        }

        auto involvedNamespaces = pipelineStatus.getValue();
        std::vector<NamespaceString> refs(involvedNamespaces.begin(), involvedNamespaces.end());
        refs.push_back(viewDef.viewOn());

        int pipelineSize = 0;
        for (const auto& obj : viewDef.pipeline()) {
            pipelineSize += obj.objsize();
        }

        if (needsValidation) {
            // Check the collation of all the dependent namespaces before updating the graph.
            auto collationStatus = _validateCollation(opCtx, viewDef, refs);
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
        _viewGraphNeedsRefresh = false;
    }

    // Remove the view definition first in case this is an update. If it is not in the graph, it
    // is simply a no-op.
    _viewGraph.remove(viewDef.name());

    return doInsert(viewDef, needsValidation);
}

Status ViewsForDatabase::_upsertIntoCatalog(OperationContext* opCtx,
                                            const CollectionPtr& systemViews,
                                            const ViewDefinition& view) {
    // Build the BSON definition for this view to be saved in the durable view catalog and/or to
    // insert in the viewMap. If the collation is empty, omit it from the definition altogether.
    BSONObjBuilder viewDefBuilder;
    viewDefBuilder.append("_id", NamespaceStringUtil::serialize(view.name()));
    viewDefBuilder.append("viewOn", view.viewOn().coll());
    viewDefBuilder.append("pipeline", view.pipeline());
    if (auto collator = view.defaultCollator()) {
        viewDefBuilder.append("collation", collator->getSpec().toBSON());
    }
    auto viewObj = viewDefBuilder.obj();

    auto id = find(opCtx, systemViews, view.name());
    Snapshotted<BSONObj> oldView;
    if (!id.isValid() || !systemViews->findDoc(opCtx, id, &oldView)) {
        LOGV2_DEBUG(22544,
                    2,
                    "Insert view to system views catalog",
                    "view"_attr = view.name(),
                    "viewCatalog"_attr = systemViews->ns());

        if (auto status = collection_internal::insertDocument(
                opCtx, systemViews, InsertStatement{viewObj}, &CurOp::get(opCtx)->debug());
            !status.isOK()) {
            return status;
        }
    } else {
        CollectionUpdateArgs args(oldView.value());
        args.criteria = BSON("_id" << NamespaceStringUtil::serialize(view.name()));
        args.update = viewObj;

        collection_internal::updateDocument(opCtx,
                                            systemViews,
                                            id,
                                            oldView,
                                            viewObj,
                                            collection_internal::kUpdateAllIndexes,
                                            &CurOp::get(opCtx)->debug(),
                                            &args);
    }

    return Status::OK();
}

void ViewsForDatabase::remove(OperationContext* opCtx,
                              const CollectionPtr& systemViews,
                              const NamespaceString& ns) {
    dassert(opCtx->lockState()->isDbLockedForMode(systemViews->ns().dbName(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(systemViews->ns(), MODE_X));

    _viewGraph.remove(ns);
    _viewMap.erase(ns.coll());
    _stats = {};

    auto id = find(opCtx, systemViews, ns);
    if (!id.isValid()) {
        return;
    }

    LOGV2_DEBUG(22545,
                2,
                "Remove view from system views catalog",
                "view"_attr = ns,
                "viewCatalog"_attr = systemViews->ns());

    collection_internal::deleteDocument(
        opCtx, systemViews, kUninitializedStmtId, id, &CurOp::get(opCtx)->debug());
}

void ViewsForDatabase::clear(OperationContext* opCtx) {
    for (auto&& [name, view] : _viewMap) {
        audit::logDropView(opCtx->getClient(),
                           view->name(),
                           view->viewOn().ns(),
                           view->pipeline(),
                           ErrorCodes::OK);
    }

    _viewMap.clear();
    _viewGraph.clear();
    _valid = true;
    _viewGraphNeedsRefresh = false;
    _stats = {};
}

Status ViewsForDatabase::_validateCollation(OperationContext* opCtx,
                                            const ViewDefinition& view,
                                            const std::vector<NamespaceString>& refs) const {
    for (auto&& potentialViewNss : refs) {
        auto otherView = lookup(potentialViewNss);
        if (otherView &&
            !CollatorInterface::collatorsMatch(view.defaultCollator(),
                                               otherView->defaultCollator())) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "View " << view.name().toStringForErrorMsg()
                                  << " has conflicting collation with view "
                                  << otherView->name().toStringForErrorMsg()};
        }
    }

    return Status::OK();
}

}  // namespace mongo
