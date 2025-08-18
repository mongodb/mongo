/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * Represents a $replaceRoot pipeline stage that can be translated to SBE instead of executing as a
 * DocumentSourceSingleDocumentTransformation.
 */
class DocumentSourceInternalReplaceRoot final : public DocumentSource {
public:
    static constexpr StringData kStageNameInternal = "$_internalReplaceRoot"_sd;

    DocumentSourceInternalReplaceRoot(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                      boost::intrusive_ptr<Expression> newRoot)
        : DocumentSource(kStageNameInternal, pExpCtx), _newRoot(newRoot) {}

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {};

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.canSwapWithSkippingOrLimitingStage = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;


    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    boost::intrusive_ptr<Expression> newRootExpression() const {
        return _newRoot;
    }

private:
    // The parsed "newRoot" argument to the $replaceRoot stage.
    boost::intrusive_ptr<Expression> _newRoot;
};
}  // namespace mongo
