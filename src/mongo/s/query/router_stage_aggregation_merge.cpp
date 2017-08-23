/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/query/router_stage_aggregation_merge.h"

#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

RouterStageAggregationMerge::RouterStageAggregationMerge(
    std::unique_ptr<Pipeline, Pipeline::Deleter> mergePipeline)
    : RouterExecStage(mergePipeline->getContext()->opCtx),
      _mergePipeline(std::move(mergePipeline)) {}

StatusWith<ClusterQueryResult> RouterStageAggregationMerge::next() {
    // Pipeline::getNext will return a boost::optional<Document> or boost::none if EOF.
    if (auto result = _mergePipeline->getNext()) {
        return {result->toBson()};
    }

    // If we reach this point, we have hit EOF.
    _mergePipeline.get_deleter().dismissDisposal();
    _mergePipeline->dispose(getOpCtx());

    return {ClusterQueryResult()};
}

void RouterStageAggregationMerge::doReattachToOperationContext() {
    _mergePipeline->reattachToOperationContext(getOpCtx());
}

void RouterStageAggregationMerge::doDetachFromOperationContext() {
    _mergePipeline->detachFromOperationContext();
}

void RouterStageAggregationMerge::kill(OperationContext* opCtx) {
    _mergePipeline.get_deleter().dismissDisposal();
    _mergePipeline->dispose(opCtx);
}

bool RouterStageAggregationMerge::remotesExhausted() {
    const auto mergeSource =
        static_cast<DocumentSourceMergeCursors*>(_mergePipeline->getSources().front().get());
    return mergeSource->remotesExhausted();
}

Status RouterStageAggregationMerge::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return {ErrorCodes::InvalidOptions, "maxTimeMS is not valid for aggregation getMore"};
}

}  // namespace mongo
