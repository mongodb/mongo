// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>
#include <vector>

namespace mongo::stage_builder {

class PlanStageSlots;

/**
 * Generates an SBE plan stage sub-tree implementing a collection scan. 'fields' can be used to
 * specify top-level fields that should be retrieved during the scan. For each name in 'fields',
 * there will be a corresponding kField slot in the PlanStageSlots object returned with the same
 * name.
 *
 * On success, a tuple containing the following data is returned:
 *   * A slot to access a fetched document (a resultSlot)
 *   * A slot to access the doc's RecordId (a recordIdSlot)
 *   * An optional slot to access the latest oplog timestamp ("ts" field) for oplog scans that were
 *     requested to track this data or that are clustered scans ("ts" is the oplog clustering key).
 *   * A generated PlanStage sub-tree.
 *
 * In cases of an error, throws.
 */
std::pair<SbStage, PlanStageSlots> generateCollScan(StageBuilderState& state,
                                                    const CollectionPtr& collection,
                                                    const CollectionScanNode* csn,
                                                    std::vector<std::string> scanFieldNames);

}  // namespace mongo::stage_builder
