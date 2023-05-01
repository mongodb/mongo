/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

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
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> scanFieldNames,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch);

}  // namespace mongo::stage_builder
