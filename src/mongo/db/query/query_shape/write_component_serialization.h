// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::query_shape {

/** Serializes the query component of a parsed update or delete for inclusion in a query shape. */
BSONObj serializeWriteQuery(const ParsedUpdate& parsedUpdate, const SerializationOptions& opts);
BSONObj serializeWriteQuery(const ParsedDelete& parsedDelete, const SerializationOptions& opts);

BSONObj serializeProjection(const projection_ast::Projection* projection,
                            const SerializationOptions& opts);

/** Serializes the update ('u') component, preserving its object/array/string BSON type. */
Value serializeUpdate(const ParsedUpdate& parsedUpdate, const SerializationOptions& opts);

/** Serializes pipeline update constants ('c'), retaining variable names. */
boost::optional<BSONObj> serializeUpdateConstants(const ParsedUpdate& parsedUpdate,
                                                  const SerializationOptions& opts);

/** Serializes modifier array filters in their parsed deterministic order. */
boost::optional<std::vector<BSONObj>> serializeArrayFilters(const ParsedUpdate& parsedUpdate,
                                                            const SerializationOptions& opts);

}  // namespace mongo::query_shape
