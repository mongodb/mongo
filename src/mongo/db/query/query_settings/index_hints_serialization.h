/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/serialization_context.h"

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
               StringData fieldName,
               BSONObjBuilder* builder,
               const SerializationContext& context);

IndexHintSpecs parse(boost::optional<TenantId> tenantId,
                     const BSONElement& element,
                     const SerializationContext& context);
}  // namespace index_hints
}  // namespace mongo::query_settings
