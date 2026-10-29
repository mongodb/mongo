// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/search/internal_search_mongot_remote_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::exec::agg {

class SearchMetaStage final : public InternalSearchMongotRemoteStage {
public:
    // Same construction API as the parent class.
    using InternalSearchMongotRemoteStage::InternalSearchMongotRemoteStage;

private:
    GetNextResult getNextAfterSetup() override;

    std::unique_ptr<executor::TaskExecutorCursor> establishCursor() override;

    bool _returnedAlready = false;
};

}  // namespace mongo::exec::agg
