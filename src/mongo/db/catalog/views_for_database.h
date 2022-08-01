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

#pragma once

#include <functional>

#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/views/durable_view_catalog.h"
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
    using ViewMap = stdx::unordered_map<NamespaceString, std::shared_ptr<ViewDefinition>>;
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

    /**
     * Helper method to build a collator from its spec.
     */
    static StatusWith<std::unique_ptr<CollatorInterface>> parseCollator(OperationContext* opCtx,
                                                                        BSONObj collationSpec);

    std::shared_ptr<DurableViewCatalog> durable;
    ViewMap viewMap;
    bool valid = false;
    ViewGraph viewGraph;
    bool viewGraphNeedsRefresh = true;
    Stats stats;
    bool ignoreExternalChange = false;

    /**
     * uasserts with the InvalidViewDefinition error if the current in-memory state of the views for
     * this database is invalid which can happen as a result of direct writes to the 'system.views'
     * collection or data corruption. This prevents further use of views on this database until the
     * issue is resolved.
     */
    void requireValidCatalog() const;

    /**
     * Returns the 'ViewDefiniton' assocated with namespace 'ns' if one exists, nullptr otherwise.
     */
    std::shared_ptr<const ViewDefinition> lookup(const NamespaceString& ns) const;

    /**
     * Reloads the views for this database by iterating the DurableViewCatalog.
     */
    Status reload(OperationContext* opCtx);

    /**
     * Inserts the view into the view map.
     */
    Status insert(OperationContext* opCtx,
                  const BSONObj& view,
                  const boost::optional<TenantId>& tenantId);

    /**
     * Returns Status::OK if each view namespace in 'refs' has the same default collation as
     * 'view'. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
     */
    Status validateCollation(OperationContext* opCtx,
                             const ViewDefinition& view,
                             const std::vector<NamespaceString>& refs) const;

    /**
     * Parses the view definition pipeline, attempts to upsert into the view graph, and
     * refreshes the graph if necessary. Returns an error status if the resulting graph
     * would be invalid. needsValidation can be set to false if the view already exists in the
     * durable view catalog and skips checking that the resulting dependency graph is acyclic and
     * within the maximum depth.
     */
    Status upsertIntoGraph(OperationContext* opCtx,
                           const ViewDefinition& viewDef,
                           const PipelineValidatorFn&,
                           bool needsValidation);

private:
    Status _insert(OperationContext* opCtx,
                   const BSONObj& view,
                   const boost::optional<TenantId>& tenantId);
};

}  // namespace mongo
