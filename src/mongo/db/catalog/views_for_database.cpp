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


#include "views_for_database.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

StatusWith<std::unique_ptr<CollatorInterface>> ViewsForDatabase::parseCollator(
    OperationContext* opCtx, BSONObj collationSpec) {
    // If 'collationSpec' is empty, return the null collator, which represents the "simple"
    // collation.
    if (collationSpec.isEmpty()) {
        return {nullptr};
    }
    return CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationSpec);
}

void ViewsForDatabase::requireValidCatalog() const {
    uassert(ErrorCodes::InvalidViewDefinition,
            "Invalid view definition detected in the view catalog. Remove the invalid view "
            "manually to prevent disallowing any further usage of the view catalog.",
            valid);
}

std::shared_ptr<const ViewDefinition> ViewsForDatabase::lookup(const NamespaceString& ns) const {
    ViewMap::const_iterator it = viewMap.find(ns.ns());
    if (it != viewMap.end()) {
        return it->second;
    }
    return nullptr;
}

Status ViewsForDatabase::reload(OperationContext* opCtx) {
    try {
        durable->iterate(opCtx, [&](const BSONObj& view) { return _insert(opCtx, view); });
    } catch (const DBException& ex) {
        auto status = ex.toStatus();
        LOGV2(22547,
              "Could not load view catalog for database",
              "db"_attr = durable->getName(),
              "error"_attr = status);
        return status;
    }
    valid = true;
    return Status::OK();
}


Status ViewsForDatabase::insert(OperationContext* opCtx, const BSONObj& view) {
    auto status = _insert(opCtx, view);
    if (!status.isOK()) {
        LOGV2(5387000,
              "Could not insert view",
              "db"_attr = durable->getName(),
              "error"_attr = status);
        return status;
    }
    valid = true;
    return Status::OK();
};

Status ViewsForDatabase::_insert(OperationContext* opCtx, const BSONObj& view) {
    BSONObj collationSpec = view.hasField("collation") ? view["collation"].Obj() : BSONObj();
    auto collator = parseCollator(opCtx, collationSpec);
    if (!collator.isOK()) {
        return collator.getStatus();
    }

    NamespaceString viewName(view["_id"].str());

    auto pipeline = view["pipeline"].Obj();
    for (auto&& stage : pipeline) {
        if (BSONType::Object != stage.type()) {
            return Status(ErrorCodes::InvalidViewDefinition,
                          str::stream() << "View 'pipeline' entries must be objects, but "
                                        << viewName.toString() << " has a pipeline element of type "
                                        << stage.type());
        }
    }

    auto viewDef = std::make_shared<ViewDefinition>(viewName.dbName(),
                                                    viewName.coll(),
                                                    view["viewOn"].str(),
                                                    pipeline,
                                                    std::move(collator.getValue()));

    // Cannot have a secondary view on a system.buckets collection, only the time-series
    // collection view.
    if (viewDef->viewOn().isTimeseriesBucketsCollection() &&
        viewDef->name() != viewDef->viewOn().getTimeseriesViewNamespace()) {
        return {
            ErrorCodes::InvalidNamespace,
            "Invalid view: cannot define a view over a system.buckets namespace except by "
            "creating a time-series collection",
        };
    }

    if (!viewName.isOnInternalDb() && !viewName.isSystem()) {
        if (viewDef->timeseries()) {
            stats.userTimeseries += 1;
        } else {
            stats.userViews += 1;
        }
    } else {
        stats.internal += 1;
    }

    viewMap[viewName.ns()] = std::move(viewDef);
    return Status::OK();
}

Status ViewsForDatabase::validateCollation(OperationContext* opCtx,
                                           const ViewDefinition& view,
                                           const std::vector<NamespaceString>& refs) const {
    for (auto&& potentialViewNss : refs) {
        auto otherView = lookup(potentialViewNss);
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

Status ViewsForDatabase::upsertIntoGraph(OperationContext* opCtx,
                                         const ViewDefinition& viewDef,
                                         const PipelineValidatorFn& validatePipeline,
                                         const bool needsValidation) {
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
                                                         << viewDef.name().ns());
            }
            return pipelineStatus.getStatus();
        }

        auto involvedNamespaces = pipelineStatus.getValue();
        std::vector<NamespaceString> refs(involvedNamespaces.begin(), involvedNamespaces.end());
        refs.push_back(viewDef.viewOn());

        int pipelineSize = 0;
        for (auto obj : viewDef.pipeline()) {
            pipelineSize += obj.objsize();
        }

        if (needsValidation) {
            // Check the collation of all the dependent namespaces before updating the graph.
            auto collationStatus = validateCollation(opCtx, viewDef, refs);
            if (!collationStatus.isOK()) {
                return collationStatus;
            }
            return viewGraph.insertAndValidate(viewDef, refs, pipelineSize);
        } else {
            viewGraph.insertWithoutValidating(viewDef, refs, pipelineSize);
            return Status::OK();
        }
    };

    if (viewGraphNeedsRefresh) {
        viewGraph.clear();
        for (auto&& iter : viewMap) {
            auto status = doInsert(*(iter.second.get()), false);
            // If we cannot fully refresh the graph, we will keep '_viewGraphNeedsRefresh' true.
            if (!status.isOK()) {
                return status;
            }
        }
        // Only if the inserts completed without error will we no longer need a refresh.
        viewGraphNeedsRefresh = false;
    }

    // Remove the view definition first in case this is an update. If it is not in the graph, it
    // is simply a no-op.
    viewGraph.remove(viewDef.name());

    return doInsert(viewDef, needsValidation);
}

}  // namespace mongo
