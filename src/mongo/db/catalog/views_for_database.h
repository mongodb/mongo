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

#pragma once

#include <functional>

#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Holds all data for the views associated with a particular database.
 */
class ViewsForDatabase {
public:
    using PipelineValidatorFn = std::function<StatusWith<stdx::unordered_set<NamespaceString>>(
        OperationContext*, const ViewDefinition&)>;

    /**
     * Usage statistics about the views associated with a single database.
     * Total views = internal + userViews + userTimeseries.
     */
    struct Stats {
        int userViews = 0;
        int userTimeseries = 0;
        int internal = 0;
    };

    enum class Durability {
        // The view is not yet inserted into the system.views collection.
        kNotYetDurable,

        // The view is already present in the system.views collection.
        kAlreadyDurable,
    };

    bool valid() const {
        return _valid;
    }

    Stats stats() const {
        return _stats;
    }

    std::shared_ptr<const ViewDefinition> lookup(const NamespaceString& ns) const;

    void iterate(const std::function<bool(const ViewDefinition& view)>& callback) const;

    /**
     * Reloads views from the system.views collection.
     */
    Status reload(OperationContext* opCtx, const CollectionPtr& systemViews);

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& systemViews,
                  const NamespaceString& viewName,
                  const NamespaceString& viewOn,
                  const BSONArray& pipeline,
                  const PipelineValidatorFn& validatePipeline,
                  const BSONObj& collator,
                  Durability durability);

    Status update(OperationContext* opCtx,
                  const CollectionPtr& systemViews,
                  const NamespaceString& viewName,
                  const NamespaceString& viewOn,
                  const BSONArray& pipeline,
                  const PipelineValidatorFn& validatePipeline,
                  std::unique_ptr<CollatorInterface> collator);

    void remove(OperationContext* opCtx,
                const CollectionPtr& systemViews,
                const NamespaceString& ns);

    void clear(OperationContext* opCtx);

private:
    /**
     * Inserts or updates the given view into the view map.
     */
    Status _upsertIntoMap(OperationContext* opCtx, std::shared_ptr<ViewDefinition> view);

    /**
     * Parses the view definition pipeline, attempts to upsert into the view graph, and refreshes
     * the graph if necessary. Returns an error status if the resulting graph would be invalid.
     * needsValidation controls whether we check that the resulting dependency graph is acyclic and
     * within the maximum depth.
     */
    Status _upsertIntoGraph(OperationContext* opCtx,
                            const ViewDefinition& viewDef,
                            const PipelineValidatorFn& validatePipeline,
                            bool needsValidation);

    /**
     * Inserts or updates the given view into the system.views collection.
     */
    Status _upsertIntoCatalog(OperationContext* opCtx,
                              const CollectionPtr& systemViews,
                              const ViewDefinition& view);

    /**
     * Returns OK if each view namespace in 'refs' has the same default collation as the given view.
     * Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
     */
    Status _validateCollation(OperationContext* opCtx,
                              const ViewDefinition& view,
                              const std::vector<NamespaceString>& refs) const;

    StringMap<std::shared_ptr<ViewDefinition>> _viewMap;
    ViewGraph _viewGraph;

    bool _valid = false;
    bool _viewGraphNeedsRefresh = true;

    Stats _stats;
};

}  // namespace mongo
