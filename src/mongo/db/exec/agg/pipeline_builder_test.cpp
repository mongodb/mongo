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

#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::test {

// This is a very simple fake subclass of 'DocumentSource'.
class DocumentSourceFake : public DocumentSource {
public:
    DocumentSourceFake()
        : DocumentSource("$fake", make_intrusive<ExpressionContextForTest>()),
          mockConstraints(StreamType::kStreaming,
                          PositionRequirement::kNone,
                          HostTypeRequirement::kNone,
                          DiskUseRequirement::kNoDiskUse,
                          FacetRequirement::kAllowed,
                          TransactionRequirement::kAllowed,
                          LookupRequirement::kAllowed,
                          UnionRequirement::kAllowed) {}

    const char* getSourceName() const final {
        return nullptr;
    };

    GetNextResult doGetNext() final {
        return GetNextResult::makeEOF();
    }

    StageConstraints constraints(
        Pipeline::SplitState = Pipeline::SplitState::kUnsplit) const final {
        return mockConstraints;
    }

    Id getId() const final {
        return 0;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final {
        return Value();
    }

private:
    StageConstraints mockConstraints;
};

TEST(PipelineBuilderTest, OneStagePipeline) {
    auto dsFake = make_intrusive<DocumentSourceFake>();
    std::list<boost::intrusive_ptr<DocumentSource>> sources{dsFake};

    auto pl = exec::agg::buildPipeline(sources);

    ASSERT_EQ(1UL, pl->getStages().size());

    // The next assertion is only true for trivial (same object instance) mapping of not-yet
    // refactored document sources during SPM-4106. After all stages are refactored, we must use
    // more specific assertions.
    ASSERT_EQ(dsFake.get(), pl->getStages().back().get());
}
}  // namespace mongo::test
