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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/match_processor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

class MatchStage final : public Stage {

public:
    MatchStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               const std::shared_ptr<MatchProcessor>& matchProcessor,
               bool isTextQuery);

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

private:
    GetNextResult doGetNext() override;

    std::shared_ptr<MatchProcessor> _matchProcessor;

    // Tracks memory used while evaluating the match expression. Reports to the operation-wide
    // tracker so all stages contribute to the operation memory total.
    SimpleMemoryUsageTracker _memoryTracker;
    // Whether to charge expression evaluation against the memory tracker. Evaluated once at
    // construction; feature flags must not change during stage execution.
    bool _trackMemory{false};
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
