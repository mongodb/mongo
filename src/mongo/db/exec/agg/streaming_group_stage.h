// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/group_base_stage.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

class StreamingGroupStage final : public mongo::exec::agg::GroupBaseStage {

public:
    StreamingGroupStage(std::string_view stageName,
                        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                        const std::shared_ptr<GroupProcessor>& groupProcessor,
                        const std::vector<size_t>& monotonicExpressionIndexes);

private:
    GetNextResult doGetNext() final;

    GetNextResult getNextDocument();

    GetNextResult readyNextBatch();
    /**
     * Readies next batch after all children are initialized. See readyNextBatch() for
     * more details.
     */
    GetNextResult readyNextBatchInner(GetNextResult input);

    template <typename IdValueGetter>
    bool checkForBatchEndAndUpdateLastIdValues(const IdValueGetter& idValueGetter);

    bool isBatchFinished(const Value& id);

    std::vector<Value> _lastMonotonicIdFieldValues;

    boost::optional<Document> _firstDocumentOfNextBatch;

    bool _sourceDepleted;

    std::vector<size_t> _monotonicExpressionIndexes;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
