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

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

namespace mongo::test {

using namespace exec::agg;

class DocumentSourceMock1 : public DocumentSourceTestOptimizations {
public:
    using DocumentSourceTestOptimizations::DocumentSourceTestOptimizations;
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

ALLOCATE_DOCUMENT_SOURCE_ID(documentSourceMock1, DocumentSourceMock1::id);

// Stage that checks it is disposed when it goes out of scope, otherwise it triggers a test failure.
class CustomDisposeStage : public Stage {
public:
    CustomDisposeStage(boost::intrusive_ptr<ExpressionContext> expCtx)
        : Stage("$customDispose", expCtx) {}

    ~CustomDisposeStage() override {
        ASSERT_EQ(true, disposed);
    }

    Stage* getSource() const {
        return pSource;
    }

    GetNextResult doGetNext() final {
        return GetNextResult::makeEOF();
    }

private:
    void doDispose() final {
        disposed = true;
    }

    bool disposed{false};
};

boost::intrusive_ptr<exec::agg::Stage> documentSourceMock1ToCustomDisposeStageMappingFn(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    return make_intrusive<CustomDisposeStage>(ds->getExpCtx());
}

REGISTER_AGG_STAGE_MAPPING(customDisposeStage,
                           DocumentSourceMock1::id,
                           documentSourceMock1ToCustomDisposeStageMappingFn);

// There is no stage associated with this DS, an exception is thrown by the mapping function.
class DocumentSourceMock2 : public DocumentSourceTestOptimizations {
public:
    using DocumentSourceTestOptimizations::DocumentSourceTestOptimizations;
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

ALLOCATE_DOCUMENT_SOURCE_ID(documentSourceMock2, DocumentSourceMock2::id);

boost::intrusive_ptr<exec::agg::Stage> documentSourceMock2ToThrowsStageMappingFn(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    uasserted(ErrorCodes::InternalError, "Mocked error");
}

REGISTER_AGG_STAGE_MAPPING(ThrowsStage,
                           DocumentSourceMock2::id,
                           documentSourceMock2ToThrowsStageMappingFn);

class DocumentSourceMock3 : public DocumentSourceTestOptimizations {
public:
    using DocumentSourceTestOptimizations::DocumentSourceTestOptimizations;
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

ALLOCATE_DOCUMENT_SOURCE_ID(documentSourceMock3, DocumentSourceMock3::id);

exec::agg::StageExpansion documentSourceMock3ToTwoStagesMappingFn(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    exec::agg::StageExpansion stages;
    stages.push_back(make_intrusive<CustomDisposeStage>(ds->getExpCtx()));
    stages.push_back(make_intrusive<CustomDisposeStage>(ds->getExpCtx()));
    return stages;
}

REGISTER_AGG_STAGES_MAPPING(twoStagesMock3,
                            DocumentSourceMock3::id,
                            documentSourceMock3ToTwoStagesMappingFn);

TEST(PipelineBuilderTest, NStageExpansionProducesTwoExecStages) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto dsm3 = make_intrusive<DocumentSourceMock3>(expCtx);
    std::list<boost::intrusive_ptr<DocumentSource>> sources{dsm3};
    auto pipeline = mongo::Pipeline::create(std::move(sources), expCtx);
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    ASSERT_EQ(2u, execPipeline->getStages().size());
}

TEST(PipelineBuilderTest, NStageExpansionInterleavedWithSingleStage) {
    // Pipeline: Mock1 (1→1) → Mock3 (1→2) produces 3 exec stages.
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto dsm1 = make_intrusive<DocumentSourceMock1>(expCtx);
    auto dsm3 = make_intrusive<DocumentSourceMock3>(expCtx);
    std::list<boost::intrusive_ptr<DocumentSource>> sources{dsm1, dsm3};
    auto pipeline = mongo::Pipeline::create(std::move(sources), expCtx);
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    auto& stages = execPipeline->getStages();
    ASSERT_EQ(3u, stages.size());
    auto* s0 = static_cast<CustomDisposeStage*>(stages[0].get());
    auto* s1 = static_cast<CustomDisposeStage*>(stages[1].get());
    auto* s2 = static_cast<CustomDisposeStage*>(stages[2].get());
    ASSERT_EQ(nullptr, s0->getSource());
    ASSERT_EQ(s0, s1->getSource());
    ASSERT_EQ(s1, s2->getSource());
}

TEST(PipelineBuilderTest, DisposeStagesIfExceptionOccurs) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto dsm1 = make_intrusive<DocumentSourceMock1>(expCtx);
    auto dsm2 = make_intrusive<DocumentSourceMock2>(expCtx);
    std::list<boost::intrusive_ptr<DocumentSource>> sources{dsm1, dsm2};

    auto pipeline = mongo::Pipeline::create(std::move(sources), expCtx);
    // CustomDisposeStage stage is created before documentSourceMock2ToThrowsStageMappingFn() throws
    // (i.e, the second stage fails to be created). CustomDisposeStage is disposed before the
    // exception is thrown.
    ASSERT_THROWS_CODE(
        exec::agg::buildPipeline(pipeline->freeze()), DBException, ErrorCodes::InternalError);
}
}  // namespace mongo::test
