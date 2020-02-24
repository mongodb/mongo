/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/pipeline/expression_context_for_test.h"

namespace mongo {
/**
 * A dummy class for other tests to inherit from to customize the behavior of any of the virtual
 * methods from DocumentSource without having to implement all of them.
 */
class DocumentSourceTestOptimizations : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalTestOptimizations"_sd;
    DocumentSourceTestOptimizations(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(DocumentSourceTestOptimizations::kStageName, expCtx) {}
    virtual ~DocumentSourceTestOptimizations() = default;
    virtual GetNextResult doGetNext() override {
        MONGO_UNREACHABLE;
    }
    virtual StageConstraints constraints(Pipeline::SplitState) const override {
        // Return the default constraints so that this can be used in test pipelines. Constructing a
        // pipeline needs to do some validation that depends on this.
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    virtual boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    virtual GetModPathsReturn getModifiedPaths() const override {
        MONGO_UNREACHABLE;
    }

private:
    virtual Value serialize(boost::optional<ExplainOptions::Verbosity>) const override {
        MONGO_UNREACHABLE;
    }
};

}  // namespace mongo
