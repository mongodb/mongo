// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] view_catalog_helpers {

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
StatusWith<ResolvedNamespace> resolveView(OperationContext* opCtx,
                                          std::shared_ptr<const CollectionCatalog> catalog,
                                          const NamespaceString& nss,
                                          boost::optional<BSONObj> timeseriesCollator);

}  // namespace view_catalog_helpers
}  // namespace mongo
