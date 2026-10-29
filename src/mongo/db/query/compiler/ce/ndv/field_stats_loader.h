// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats_gen.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>


namespace mongo::ce {

/**
 * Loads the persisted statistics for one combination of field paths from
 * <dbName>.system.stats.field_stats.
 *
 * The lookup is a point _id query (see makeFieldStatsId()) with no projection, which keeps it
 * express-eligible. Returns NoSuchKey when no statistics are persisted for the key. A document
 * that does not match the FieldStatsDoc schema yields an error status rather than an exception:
 * malformed statistics must degrade to "no statistics", never fail the caller's operation.
 */
StatusWith<FieldStatsDoc> loadFieldStats(OperationContext* opCtx,
                                         const DatabaseName& dbName,
                                         const UUID& collectionUuid,
                                         std::vector<std::string> fieldPaths);

}  // namespace mongo::ce
