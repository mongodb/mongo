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
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/string_map.h"

namespace mongo {
class OperationContext;
class Database;

/**
 * In-memory data structure for view definitions. Instances returned by get() are immutable,
 * modifications through the static functions copy the existing instance and perform the
 * modification on the copy. A new call to get() is necessary to observe the modification.
 *
 * Writes via the static functions are thread-safe and serialized with a mutex.
 *
 * The static methods refresh the in-memory map with the views catalog collection if necessary,
 * throwing if the refresh fails.
 */
class ViewCatalog {
public:
    using ViewMap = StringMap<std::shared_ptr<ViewDefinition>>;
    using ViewIteratorCallback = std::function<bool(const ViewDefinition& view)>;

    static std::shared_ptr<const ViewCatalog> get(ServiceContext* svcCtx);
    static std::shared_ptr<const ViewCatalog> get(OperationContext* opCtx);

    /**
     * Add an entry to the ViewCatalog for the given database, backed by the durable storage
     * 'catalog'.
     */
    static Status registerDatabase(OperationContext* opCtx,
                                   StringData dbName,
                                   std::unique_ptr<DurableViewCatalog> catalog);

    /**
     * Removes the ViewCatalog entries assocated with 'db' if any. Should be called when when a
     * `DatabaseImpl` that has previously registered is about to be destructed (e.g. when closing a
     * database).
     */
    static void unregisterDatabase(OperationContext* opCtx, Database* db);

    /**
     * Iterates through the catalog, applying 'callback' to each view. This callback function
     * executes under the catalog's mutex, so it must not access other methods of the catalog,
     * acquire locks or run for a long time. If the 'callback' returns false, the iterator exits
     * early.
     *
     * Caller must ensure corresponding database exists.
     */
    void iterate(StringData dbName, ViewIteratorCallback callback) const;

    /**
     * Create a new view 'viewName' with contents defined by running the specified aggregation
     * 'pipeline' with collation 'collation' on a collection or view 'viewOn'. This method will
     * check correctness with respect to the view catalog, but will not check for conflicts with the
     * database's catalog, so the check for an existing collection with the same name must be done
     * before calling createView.
     *
     * Must be in WriteUnitOfWork. View creation rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists.
     */
    static Status createView(OperationContext* opCtx,
                             const NamespaceString& viewName,
                             const NamespaceString& viewOn,
                             const BSONArray& pipeline,
                             const BSONObj& collation);

    /**
     * Drop the view named 'viewName'.
     *
     * Must be in WriteUnitOfWork. The drop rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists.
     */
    static Status dropView(OperationContext* opCtx, const NamespaceString& viewName);

    /**
     * Modify the view named 'viewName' to have the new 'viewOn' and 'pipeline'.
     *
     * Must be in WriteUnitOfWork. The modification rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists.
     */
    static Status modifyView(OperationContext* opCtx,
                             const NamespaceString& viewName,
                             const NamespaceString& viewOn,
                             const BSONArray& pipeline);

    /**
     * Look up the 'nss' in the view catalog, returning a shared pointer to a View definition, or
     * nullptr if it doesn't exist.
     *
     * Caller must ensure corresponding database exists.
     */
    std::shared_ptr<const ViewDefinition> lookup(OperationContext* opCtx,
                                                 const NamespaceString& nss) const;

    /**
     * Same functionality as above, except this function skips validating durable views in the view
     * catalog.
     *
     * Caller must ensure corresponding database exists.
     */
    std::shared_ptr<const ViewDefinition> lookupWithoutValidatingDurableViews(
        OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Resolve the views on 'nss', transforming the pipeline appropriately. This function returns a
     * fully-resolved view definition containing the backing namespace, the resolved pipeline and
     * the collation to use for the operation.
     *
     * With SERVER-54597, we allow queries on timeseries collections *only* to specify non-default
     * collations. So in the case of queries on timeseries collections, we create a ResolvedView
     * with the request's collation (timeSeriesCollator) rather than the collection's default
     * collator.
     *
     * Caller must ensure corresponding database exists.
     */
    StatusWith<ResolvedView> resolveView(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         boost::optional<BSONObj> timeseriesCollator) const;

    /**
     * Usage statistics about this view catalog.
     * Total views = internal + userViews + userTimeseries.
     */
    struct Stats {
        int userViews = 0;
        int userTimeseries = 0;
        int internal = 0;
    };

    /**
     * Returns view statistics for the specified database.
     */
    boost::optional<Stats> getStats(StringData dbName) const;

    /**
     * Returns Status::OK with the set of involved namespaces if the given pipeline is eligible to
     * act as a view definition. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
     */
    static StatusWith<stdx::unordered_set<NamespaceString>> validatePipeline(
        OperationContext* opCtx, const ViewDefinition& viewDef);

    /**
     * Reloads the in-memory state of the view catalog from the 'system.views' collection catalog.
     * If the 'lookupBehavior' is 'kValidateDurableViews', then the durable view definitions will be
     * validated. Reading stops on the first invalid entry with errors logged and returned. Performs
     * no cycle detection, etc.
     * This is implicitly called by other methods when write operations are performed on the view
     * catalog, on external changes to the 'system.views' collection and on the first opening of a
     * database.
     */
    static Status reload(OperationContext* opCtx,
                         StringData dbName,
                         ViewCatalogLookupBehavior lookupBehavior);

    /**
     * Clears the in-memory state of the view catalog.
     */
    static void clear(OperationContext* opCtx, StringData dbName);

    /**
     * The view catalog needs to ignore external changes for its own modifications.
     */
    static bool shouldIgnoreExternalChange(OperationContext* opCtx, const NamespaceString& name);

private:
    Status _createOrUpdateView(OperationContext* opCtx,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline,
                               std::unique_ptr<CollatorInterface> collator);
    /**
     * Parses the view definition pipeline, attempts to upsert into the view graph, and refreshes
     * the graph if necessary. Returns an error status if the resulting graph would be invalid.
     */
    Status _upsertIntoGraph(OperationContext* opCtx, const ViewDefinition& viewDef);

    /**
     * Returns Status::OK if each view namespace in 'refs' has the same default collation as 'view'.
     * Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
     */
    Status _validateCollation(OperationContext* opCtx,
                              const ViewDefinition& view,
                              const std::vector<NamespaceString>& refs) const;

    std::shared_ptr<const ViewDefinition> _lookup(OperationContext* opCtx,
                                                  const NamespaceString& ns,
                                                  ViewCatalogLookupBehavior lookupBehavior) const;
    std::shared_ptr<ViewDefinition> _lookup(OperationContext* opCtx,
                                            const NamespaceString& ns,
                                            ViewCatalogLookupBehavior lookupBehavior);

    Status _reload(OperationContext* opCtx,
                   StringData dbName,
                   ViewCatalogLookupBehavior lookupBehavior,
                   bool reloadForCollectionCatalog);

    /**
     * Holds all data for the views associated with a particular database. Prior to 5.3, the
     * ViewCatalog object was owned by the Database object as a decoration. It has now transitioned
     * to a global catalog, as a decoration on the ServiceContext. Each database gets its own record
     * here, comprising the same information that was previously stored as top-level information
     * prior to 5.3.
     */
    struct ViewsForDatabase {
        ViewMap viewMap;
        std::shared_ptr<DurableViewCatalog> durable;
        bool valid = false;
        ViewGraph viewGraph;
        bool viewGraphNeedsRefresh = true;
        Stats stats;

        /**
         * uasserts with the InvalidViewDefinition error if the current in-memory state of the view
         * catalog for the given database is invalid. This ensures that calling into the view
         * catalog while it is invalid renders it inoperable.
         */
        void requireValidCatalog() const;
    };
    StringMap<ViewsForDatabase> _viewsForDatabase;
};
}  // namespace mongo
