/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"

#include <memory>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class VectorSearchStage final : public Stage {
public:
    static constexpr StringData kNumCandidatesFieldName = "numCandidates"_sd;

    VectorSearchStage(StringData stageName,
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
