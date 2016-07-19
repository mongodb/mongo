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

#pragma once

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
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {
class OperationContext;

/**
 * In-memory data structure for view definitions. This datastructure is thread-safe. This is needed
 * as concurrent updates may happen through direct writes to the views catalog collection.
 */
class ViewCatalog {
    MONGO_DISALLOW_COPYING(ViewCatalog);

public:
    // TODO(SERVER-23700): Make this a unique_ptr once StringMap supports move-only types.
    using ViewMap = StringMap<std::shared_ptr<ViewDefinition>>;

    explicit ViewCatalog(DurableViewCatalog* durable) : _durable(durable) {}

    ViewMap::const_iterator begin() const {
        return _viewMap.begin();
    }

    ViewMap::const_iterator end() const {
        return _viewMap.end();
    }

    /**
     * Create a new view 'viewName' with contents defined by running the specified aggregation
     * 'pipeline' on a collection or view 'viewOn'. This method will check correctness with
     * respect to the view catalog, but will not check for conflicts with the database's catalog,
     * so the check for an existing collection with the same name must be done before calling
     * createView.
     *
     * Must be in WriteUnitOfWork. View creation rolls back if the unit of work aborts.
     */
    Status createView(OperationContext* txn,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline);

    /**
     * Drop the view named 'viewName'.
     *
     * Must be in WriteUnitOfWork. The drop rolls back if the unit of work aborts.
     */
    Status dropView(OperationContext* txn, const NamespaceString& viewName);

    /**
     * Modify the view named 'viewName' to have the new 'viewOn' and 'pipeline'.
     *
     * Must be in WriteUnitOfWork. The modification rolls back if the unit of work aborts.
     */
    Status modifyView(OperationContext* txn,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline);


    /**
     * Look up the namespace in the view catalog, returning a pointer to a View definition, or
     * nullptr if it doesn't exist. Note that the caller does not own the pointer.
     *
     * @param ns The full namespace string of the view.
     * @return A bare pointer to a view definition if ns is a valid view with a backing namespace.
     */
    ViewDefinition* lookup(OperationContext* txn, StringData ns);

    /**
     * Resolve the views on 'ns', transforming the pipeline appropriately. This function returns a
     * pair containing the fully-qualified namespace of the backing collection and the raw pipeline
     * for an aggregation.
     *
     * It is illegal to call this function on a namespace that is not a view.
     */
    StatusWith<ResolvedView> resolveView(OperationContext* txn, const NamespaceString& nss);

    /**
     * Reload the views catalog if marked invalid. No-op if already valid. Does only minimal
     * validation, namely that the view definitions are valid BSON and have no unknown fields.
     * No cycle detection etc. This is implicitly called by other methods when the ViewCatalog is
     * marked invalid, and on first opening a database.
     */
    Status reloadIfNeeded(OperationContext* txn);

    /**
     * To be called when direct modifications to the DurableViewCatalog have been committed, so
     * subsequent lookups will reload the catalog and make the changes visible.
     */
    void invalidate() {
        _valid.store(false);
        _viewGraphNeedsRefresh = true;
    }

private:
    Status _createOrUpdateView_inlock(OperationContext* txn,
                                      const NamespaceString& viewName,
                                      const NamespaceString& viewOn,
                                      const BSONArray& pipeline);
    /**
     * Parses the view definition pipeline, attempts to upsert into the view graph, and refreshes
     * the graph if necessary. Returns an error status if the resulting graph would be invalid.
     */
    Status _upsertIntoGraph(OperationContext* txn, const ViewDefinition& viewDef);

    ViewDefinition* _lookup_inlock(OperationContext* txn, StringData ns);
    Status _reloadIfNeeded_inlock(OperationContext* txn);

    stdx::mutex _mutex;  // Protects all members, except for _valid.
    ViewMap _viewMap;
    DurableViewCatalog* _durable;
    AtomicBool _valid;
    ViewGraph _viewGraph;
    bool _viewGraphNeedsRefresh = true;  // Defers initializing the graph until the first insert.
};
}  // namespace mongo
