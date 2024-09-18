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

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {
namespace view_catalog_helpers {

/**
 * Returns Status::OK with the set of involved namespaces if the given pipeline is eligible to
 * act as a view definition. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
 */
StatusWith<stdx::unordered_set<NamespaceString>> validatePipeline(OperationContext* opCtx,
                                                                  const ViewDefinition& viewDef);

/**
 * Resolve the views on 'nss', transforming the pipeline appropriately. This function returns a
 * fully-resolved view definition containing the backing namespace, the resolved pipeline and
 * the collation to use for the operation.
 *
 * With SERVER-54597, we allow queries on timeseries collections *only* to specify non-default
 * collations. So in the case of queries on timeseries collections, we create a ResolvedView
 * with the request's collation (timeSeriesCollator) rather than the collection's default
 * collator.
 */
StatusWith<ResolvedView> resolveView(OperationContext* opCtx,
                                     std::shared_ptr<const CollectionCatalog> catalog,
                                     const NamespaceString& nss,
                                     boost::optional<BSONObj> timeseriesCollator);

}  // namespace view_catalog_helpers
}  // namespace mongo
