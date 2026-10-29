// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {
class ListLocalSessionsStage final : public Stage {
public:
    ListLocalSessionsStage(std::string_view stageName,
                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           const ListSessionsSpec& spec);

private:
    GetNextResult doGetNext() final;

    const LogicalSessionCache* _cache;
    std::vector<LogicalSessionId> _ids;

    const ListSessionsSpec _spec;
};
}  // namespace mongo::exec::agg
