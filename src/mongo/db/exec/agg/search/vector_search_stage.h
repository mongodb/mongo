// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
using namespace std::literals::string_view_literals;

class VectorSearchStage final : public Stage {
public:
    static constexpr std::string_view kNumCandidatesFieldName = "numCandidates"sv;

    VectorSearchStage(std::string_view stageName,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      const std::shared_ptr<executor::TaskExecutor>& taskExecutor,
                      BSONObj originalSpec,
                      const std::shared_ptr<DSVectorSearchExecStatsWrapper>& execStatsWrapper);

private:
    // Get the next record from mongot. This will establish the mongot cursor on the first call.
    GetNextResult doGetNext() final;

    boost::optional<BSONObj> getNext();

    GetNextResult getNextAfterSetup();

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;
    std::unique_ptr<executor::TaskExecutorCursor> _cursor;
    std::shared_ptr<DSVectorSearchExecStatsWrapper> _execStatsWrapper;

    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    // Keep track of the original request BSONObj's extra fields in case there were fields mongod
    // doesn't know about that mongot will need later.
    const BSONObj _originalSpec;
};

}  // namespace mongo::exec::agg
