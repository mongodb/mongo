// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalAssertDataAssumptionsStage final : public Stage {
public:
    InternalAssertDataAssumptionsStage(std::string_view stageName,
                                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       std::set<FieldPath> nonArrayPaths);

    bool isEOF() const final {
        return (pSource && pSource->isEOF());
    }

private:
    GetNextResult doGetNext() final;

    std::set<FieldPath> _nonArrayPaths;
};

}  // namespace mongo::exec::agg
