// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * Execution stage backing $_internalQuerySettingsDebugShape. For each input document that carries a
 * 'representativeQuery' field, appends a 'debugQueryShape' field computed from that query.
 */
class QuerySettingsDebugShapeStage final : public Stage {
public:
    static constexpr std::string_view kDebugQueryShapeFieldName = "debugQueryShape";

    QuerySettingsDebugShapeStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;
};

}  // namespace mongo::exec::agg
