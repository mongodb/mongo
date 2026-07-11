// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
