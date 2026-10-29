// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class InternalShredDocumentsStage final : public Stage {
public:
    InternalShredDocumentsStage(std::string_view stageName,
                                const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;
};

}  // namespace mongo::exec::agg
