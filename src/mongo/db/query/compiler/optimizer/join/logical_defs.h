// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <cstdint>

/** Common definitions for the graph logical model.
 */
namespace mongo::join_ordering {
/** Join Node's identifier.
 */
using NodeId = uint16_t;

/** Join Edge's identifier.
 */
using EdgeId = uint16_t;

/** Resolved Path's identifier.
 */
using PathId = uint16_t;

/** Join Predicate's unique identifier.
 */
using PredicateId = uint16_t;

struct ResolvedPath {
    NodeId nodeId;
    FieldPath fieldName;
};
}  // namespace mongo::join_ordering
