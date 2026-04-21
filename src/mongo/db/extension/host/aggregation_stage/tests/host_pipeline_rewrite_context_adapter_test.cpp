/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_dependencies_adapter.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/pipeline_dependencies_handle.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_mock_stages.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::extension {
namespace {

using namespace host_connector;
using namespace rule_based_rewrites::pipeline;

TEST(PipelineRewriteContextAdapterTest, GetNthNextStageReturnsFirstNextStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    auto stage = PipelineRewriteContextAPI(&adapter).getNthNextStage(1);
    ASSERT_NOT_EQUALS(nullptr, stage.get());
    ASSERT_EQUALS(DocumentSourceSkip::kStageName, stage->getName());
}

TEST(PipelineRewriteContextAdapterTest, GetNthNextStageReturnsSecondNextStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));
    container.push_back(DocumentSourceLimit::create(expCtx, 3));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    auto stage = api.getNthNextStage(2);
    ASSERT_NOT_EQUALS(nullptr, stage.get());
    ASSERT_EQUALS(DocumentSourceLimit::kStageName, stage->getName());
}

TEST(PipelineRewriteContextAdapterTest, GetNthNextStageResultUpdatedOnSubsequentCall) {
    // Calling getNthNextStage twice returns the correct stage for each index, verifying that the
    // cached result is replaced on each invocation.
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));
    container.push_back(DocumentSourceLimit::create(expCtx, 3));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_EQUALS(DocumentSourceSkip::kStageName, api.getNthNextStage(1)->getName());
    ASSERT_EQUALS(DocumentSourceLimit::kStageName, api.getNthNextStage(2)->getName());
}

TEST(PipelineRewriteContextAdapterTest, EraseNthNextStageRemovesFirstNextStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));
    container.push_back(DocumentSourceLimit::create(expCtx, 3));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_TRUE(api.eraseNthNext(1));

    // $skip (index 1) is gone; the only remaining next stage is the second $limit.
    ASSERT_EQUALS(2u, container.size());
    ASSERT_EQUALS(DocumentSourceLimit::kStageName, api.getNthNextStage(1)->getName());
}

TEST(PipelineRewriteContextAdapterTest, EraseNthNextStageRemovesNonAdjacentStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));
    container.push_back(DocumentSourceLimit::create(expCtx, 3));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_TRUE(api.eraseNthNext(2));

    // The second $limit (index 2) is gone; $skip (index 1) remains.
    ASSERT_EQUALS(2u, container.size());
    ASSERT_EQUALS(DocumentSourceSkip::kStageName, api.getNthNextStage(1)->getName());
}

TEST(PipelineRewriteContextAdapterTest, HasAtLeastNNextStagesTrueWhenEnoughStagesExist) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));
    container.push_back(DocumentSourceLimit::create(expCtx, 3));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_TRUE(api.hasAtLeastNNextStages(1));
    ASSERT_TRUE(api.hasAtLeastNNextStages(2));
}

TEST(PipelineRewriteContextAdapterTest, HasAtLeastNNextStagesFalseWhenNotEnoughStagesExist) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_TRUE(api.hasAtLeastNNextStages(1));
    ASSERT_FALSE(api.hasAtLeastNNextStages(2));
    ASSERT_FALSE(api.hasAtLeastNNextStages(3));
}

TEST(PipelineRewriteContextAdapterTest, HasAtLeastNNextStagesFalseAtLastStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    PipelineRewriteContextAdapter adapter(host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI api(&adapter);
    ASSERT_FALSE(api.hasAtLeastNNextStages(1));
}

TEST(PipelineRewriteContextAdapterTest, GetPipelineSuffixDependenciesEmptyAtLastStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    ASSERT_TRUE(rbrCtx.atLastStage());

    DepsTracker deps = rbrCtx.getPipelineSuffixDependencies();
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(PipelineRewriteContextAdapterTest, GetPipelineSuffixDependenciesWithMetadataRequest) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(
        DocumentSourceGeneratesMetaField::create(expCtx, DocumentMetadataFields::kTextScore));
    container.push_back(
        DocumentSourceNeedsMetaField::create(expCtx, DocumentMetadataFields::kTextScore));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    ASSERT_FALSE(rbrCtx.atLastStage());

    DepsTracker deps = rbrCtx.getPipelineSuffixDependencies();
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kSearchScore));
}

TEST(PipelineRewriteContextAdapterTest, VariableRefsEmptyForUnknownVariable) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    auto variableRefs = rbrCtx.getBuiltInVariableRefsInPipelineSuffix();
    ASSERT_FALSE(variableRefs.contains("SOME_UNKNOWN_VARIABLE"));

    // Verify the adapter agrees.
    DepsTracker deps;
    auto adapter =
        host_connector::PipelineDependenciesAdapter(std::move(deps), std::move(variableRefs));
    ASSERT_FALSE(PipelineDependenciesHandle(&adapter)->needsVariable("SOME_UNKNOWN_VARIABLE"));
}

TEST(PipelineRewriteContextAdapterTest, VariableRefsDoNotContainUnreferencedVariable) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    container.push_back(DocumentSourceSkip::create(expCtx, 5));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    // $limit and $skip don't reference $$SEARCH_META.
    auto variableRefs = rbrCtx.getBuiltInVariableRefsInPipelineSuffix();
    ASSERT_FALSE(variableRefs.contains("SEARCH_META"));

    DepsTracker deps;
    auto adapter =
        host_connector::PipelineDependenciesAdapter(std::move(deps), std::move(variableRefs));
    ASSERT_FALSE(PipelineDependenciesHandle(&adapter)->needsVariable("SEARCH_META"));
}

TEST(PipelineRewriteContextAdapterTest, VariableRefsContainSearchMetaWhenReferenced) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));
    // $addFields references $$SEARCH_META, so it appears in the suffix variable refs.
    container.push_back(DocumentSourceAddFields::create(BSON("x" << "$$SEARCH_META"), expCtx));

    PipelineRewriteContext rbrCtx(*expCtx, container);
    auto variableRefs = rbrCtx.getBuiltInVariableRefsInPipelineSuffix();
    ASSERT_TRUE(variableRefs.contains("SEARCH_META"));

    DepsTracker deps;
    auto adapter =
        host_connector::PipelineDependenciesAdapter(std::move(deps), std::move(variableRefs));
    ASSERT_TRUE(PipelineDependenciesHandle(&adapter)->needsVariable("SEARCH_META"));
}

TEST(PipelineDependenciesAdapterTest, NeedsWholeDocumentReturnsTrue) {
    DepsTracker deps;
    deps.needWholeDocument = true;
    auto adapter = host_connector::PipelineDependenciesAdapter(std::move(deps));
    ASSERT_TRUE(PipelineDependenciesHandle(&adapter)->needsWholeDocument());
}

TEST(PipelineDependenciesAdapterTest, NeedsWholeDocumentReturnsFalse) {
    DepsTracker deps;
    deps.needWholeDocument = false;
    auto adapter = host_connector::PipelineDependenciesAdapter(std::move(deps));
    ASSERT_FALSE(PipelineDependenciesHandle(&adapter)->needsWholeDocument());
}

TEST(PipelineDependenciesAdapterTest, NeedsMetadataReturnsTrueForSetMetadataType) {
    DepsTracker deps;
    deps.setNeedsMetadata(DocumentMetadataFields::kSearchScore);
    auto adapter = host_connector::PipelineDependenciesAdapter(std::move(deps));
    ASSERT_TRUE(PipelineDependenciesHandle(&adapter)->needsMetadata("searchScore"));
}

TEST(PipelineDependenciesAdapterTest, NeedsMetadataReturnsFalseForUnsetMetadataType) {
    DepsTracker deps;
    // Do not set kSearchScore.
    auto adapter = host_connector::PipelineDependenciesAdapter(std::move(deps));
    ASSERT_FALSE(PipelineDependenciesHandle(&adapter)->needsMetadata("searchScore"));
}

}  // namespace
}  // namespace mongo::extension
