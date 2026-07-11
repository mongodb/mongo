// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <string_view>

namespace mongo::query_settings {
class IndexHintSpec;

/**
 * Type alias for the collection of IndexHintSpec, which represent index hints per collection in
 * QuerySettings. As in most of the cases users will be setting index hints on a single collection,
 * the default vector size is 1.
 */
using IndexHintSpecs = absl::InlinedVector<IndexHintSpec, 1>;

namespace index_hints {
void serialize(const IndexHintSpecs& indexHints,
               std::string_view fieldName,
               BSONObjBuilder* builder,
               const SerializationContext& context);

IndexHintSpecs parse(boost::optional<TenantId> tenantId,
                     const BSONElement& element,
                     const SerializationContext& context);
}  // namespace index_hints
}  // namespace mongo::query_settings
