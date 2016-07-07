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

#include "mongo/base/status_with.h"
#include "mongo/db/views/view.h"
#include "mongo/util/string_map.h"

namespace mongo {
class AggregationRequest;
class Database;
class NamespaceString;
class OperationContext;
class Status;
class StringData;

/**
 * Represents a fully-resolved view: a non-view namespace with a corresponding aggregation pipeline.
 */
struct ResolvedViewDefinition {
    /**
     * Creates a new aggregation command object for a view operation. The new command is an
     * aggregation on 'collectionNss', and its pipeline is the concatenation of 'pipeline' with the
     * pipeline of 'request'.
     */
    BSONObj asExpandedViewAggregation(const AggregationRequest& request);

    NamespaceString collectionNss;
    std::vector<BSONObj> pipeline;
};

/**
 * In-memory data structure for view definitions. Note that this structure is not thread-safe; you
 * must be holding a database lock to access a database's view catalog.
 */
class ViewCatalog {
    MONGO_DISALLOW_COPYING(ViewCatalog);

public:
    // TODO(SERVER-23700): Make this a unique_ptr once StringMap supports move-only types.
    using ViewMap = StringMap<std::shared_ptr<ViewDefinition>>;
    static const std::uint32_t kMaxViewDepth;

    ViewCatalog(OperationContext* txn, Database* database);

    ViewMap::const_iterator begin() const {
        return _viewMap.begin();
    }

    ViewMap::const_iterator end() const {
        return _viewMap.end();
    }

    /**
     * Create a new view.
     *
     * @param viewName The name of the view being created.
     * @param viewOn The name of the view or collection upon which this view is defined.
     * @param pipeline The aggregation pipeline that defines the aggregation on the backing
     * namespace.
     */
    Status createView(OperationContext* txn,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONObj& pipeline);

    /**
     * Look up the namespace in the view catalog, returning a pointer to a View definition, or
     * nullptr if it doesn't exist. Note that the caller does not own the pointer.
     *
     * @param ns The full namespace string of the view.
     * @return A bare pointer to a view definition if ns is a valid view with a backing namespace.
     */
    ViewDefinition* lookup(StringData ns);

    /**
     * Resolve the views on 'ns', transforming the pipeline appropriately. This function returns a
     * pair containing the fully-qualified namespace of the backing collection and the raw pipeline
     * for an aggregation.
     *
     * It is illegal to call this function on a namespace that is not a view.
     */
    StatusWith<ResolvedViewDefinition> resolveView(OperationContext* txn,
                                                   const NamespaceString& nss);

private:
    ViewMap _viewMap;
};
}  // namespace mongo
