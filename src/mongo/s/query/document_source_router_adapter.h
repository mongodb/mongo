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

#pragma once

#include "mongo/s/query/router_exec_stage.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
/**
 * A class that acts as an adapter between the RouterExecStage and DocumentSource interfaces,
 * translating results from an input RouterExecStage into DocumentSource::GetNextResults.
 */
class DocumentSourceRouterAdapter final : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceRouterAdapter> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<RouterExecStage> childStage);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kFirst,
                HostTypeRequirement::kMongoS,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed};
    }

    GetNextResult getNext() final;
    void doDispose() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    void detachFromOperationContext() final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final;
    bool remotesExhausted();

    void setExecContext(RouterExecStage::ExecContext execContext) {
        _execContext = execContext;
    }

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) const {
        return _child->setAwaitDataTimeout(awaitDataTimeout);
    }

private:
    DocumentSourceRouterAdapter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                std::unique_ptr<RouterExecStage> childStage);

    std::unique_ptr<RouterExecStage> _child;
    RouterExecStage::ExecContext _execContext;
};
}  // namespace mongo
