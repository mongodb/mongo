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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <memory>
#include <utility>

namespace mongo {

using fts::FTSMatcher;
using fts::FTSQueryImpl;
using fts::FTSSpec;


class OperationContext;
class RecordID;

struct TextMatchParams {
    TextMatchParams(const IndexDescriptor* index,
                    const FTSSpec& spec,
                    BSONObj indexPrefix,
                    const FTSQueryImpl& query)
        : index(index), spec(spec), indexPrefix(std::move(indexPrefix)), query(query) {}

    // Text index descriptor.  IndexCatalog owns this.
    const IndexDescriptor* const index;

    // Index spec.
    const FTSSpec spec;

    // Index keys that precede the "text" index key.
    const BSONObj indexPrefix;

    // The text query.
    const FTSQueryImpl query;
};

/**
 * A stage that returns every document in the child that satisfies the FTS text matcher built with
 * the query parameter.
 *
 * Prerequisites: A single child stage that passes up WorkingSetMembers in the RID_AND_OBJ state.
 * Members must also have text score metadata if it is necessary for the final projection.
 */
class TextMatchStage final : public PlanStage {
public:
    TextMatchStage(ExpressionContext* expCtx,
                   std::unique_ptr<PlanStage> child,
                   const TextMatchParams& params,
                   WorkingSet* ws);
    ~TextMatchStage() override;

    void addChild(PlanStage* child);

    bool isEOF() const final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_TEXT_MATCH;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // Text-specific phrase and negated term matcher.
    FTSMatcher _ftsMatcher;

    // Not owned by us.
    WorkingSet* _ws;

    TextMatchStats _specificStats;
};
}  // namespace mongo
