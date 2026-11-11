// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream_convert_errors.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using change_stream::pipeline_helpers::addErrorConversionStageIfNeeded;

// The injection helper appends the conversion stage above every user stage of an opted-in pipeline,
// is a no-op when the option is off, and does not append a second stage when called twice.
TEST(ChangeStreamConvertErrorsInjectionTest, AppendsStageOnlyWhenOptedIn) {
    auto buildPipeline = [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return Pipeline::create({DocumentSourceMock::createForTest({Document{}}, expCtx)}, expCtx);
    };

    // Option off: the pipeline is untouched.
    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto pipeline = buildPipeline(expCtx);
        addErrorConversionStageIfNeeded(*pipeline);
        ASSERT_FALSE(pipeline->getSources().empty());
        ASSERT_FALSE(
            pipeline->getSources().back()->isInstanceOf<DocumentSourceChangeStreamConvertErrors>());
    }

    // Option on: the stage is appended above every user stage.
    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        DocumentSourceChangeStreamSpec spec;
        spec.setShowErrorsAsEvents(true);
        expCtx->setChangeStreamSpec(spec);
        auto pipeline = buildPipeline(expCtx);
        addErrorConversionStageIfNeeded(*pipeline);
        ASSERT_TRUE(
            pipeline->getSources().back()->isInstanceOf<DocumentSourceChangeStreamConvertErrors>());

        // Idempotent: calling again does not append a second stage.
        addErrorConversionStageIfNeeded(*pipeline);
        size_t count = 0;
        for (const auto& source : pipeline->getSources()) {
            count += source->isInstanceOf<DocumentSourceChangeStreamConvertErrors>();
        }
        ASSERT_EQ(count, 1u);
    }
}

}  // namespace
}  // namespace mongo
