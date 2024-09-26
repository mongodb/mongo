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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_mock_stages.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

enum class Tracking { forwards, backwards };

/**
 * Test fixture which provides an ExpressionContext for use in testing.
 */
class AggregationContextFixture : public ServiceContextTest {
public:
    struct ExpressionContextOptionsStruct {
        bool inRouter = false;
        bool allowDiskUse = true;
    };

    AggregationContextFixture()
        : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
              boost::none, "unittests", "pipeline_test")) {}

    AggregationContextFixture(NamespaceString nss) {
        _opCtx = makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), nss);
        _expCtx->tempDir = _tempDir.path();
        _expCtx->changeStreamSpec = DocumentSourceChangeStreamSpec();
    }

    auto getExpCtx() {
        return _expCtx;
    }

    auto getExpCtxRaw() {
        return _expCtx.get();
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

    void setExpCtx(ExpressionContextOptionsStruct options) {
        _expCtx->inRouter = options.inRouter;
        _expCtx->allowDiskUse = options.allowDiskUse;
    }

    /*
     * Serialize and redact a document source.
     */
    BSONObj redact(const DocumentSource& docSource,
                   bool performRedaction = true,
                   boost::optional<ExplainOptions::Verbosity> verbosity = boost::none) {
        SerializationOptions options;
        options.verbosity = verbosity;
        if (performRedaction) {
            options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
            options.transformIdentifiersCallback = [](StringData s) -> std::string {
                return str::stream() << "HASH<" << s << ">";
            };
            options.transformIdentifiers = true;
        }
        std::vector<Value> serialized;
        docSource.serializeToArray(serialized, options);
        ASSERT_EQ(1, serialized.size());
        return serialized[0].getDocument().toBson().getOwned();
    }

    std::vector<Value> redactToArray(const DocumentSource& docSource,
                                     bool performRedaction = true) {
        SerializationOptions options;
        if (performRedaction) {
            options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
            options.transformIdentifiersCallback = [](StringData s) -> std::string {
                return str::stream() << "HASH<" << s << ">";
            };
            options.transformIdentifiers = true;
        }
        std::vector<Value> serialized;
        docSource.serializeToArray(serialized, options);
        return serialized;
    }

    // Start of functions that are used for making parts of the sources for making a pipeline.
    boost::intrusive_ptr<DocumentSourceMock> mockStage() {
        return DocumentSourceMock::createForTest(_expCtx);
    }

    boost::intrusive_ptr<DocumentSourceDeferredMergeSort> mockDeferredSortStage() {
        return DocumentSourceDeferredMergeSort::create(_expCtx);
    }

    boost::intrusive_ptr<DocumentSourceMustRunOnMongoS> runOnMongos() {
        return DocumentSourceMustRunOnMongoS::create(_expCtx);
    }

    boost::intrusive_ptr<DocumentSourceMatch> matchStage(const std::string& matchStr) {
        return DocumentSourceMatch::create(fromjson(matchStr), _expCtx);
    }

    boost::intrusive_ptr<DocumentSource> splitStage(
        const StageConstraints::HostTypeRequirement& mergeType) {
        return DocumentSourceInternalSplitPipeline::create(_expCtx, mergeType);
    }

    boost::intrusive_ptr<DocumentSource> outStage() {
        auto outSpec = fromjson("{$out: 'outcoll'}");
        return DocumentSourceOut::createFromBson(outSpec["$out"], _expCtx);
    }

    boost::intrusive_ptr<DocumentSource> projectStage(const std::string& projectStr) {
        auto outSpec = fromjson("{$out: 'outcoll'}");
        return DocumentSourceOut::createFromBson(outSpec["$out"], _expCtx);
    }

    boost::intrusive_ptr<DocumentSource> lookupStage(const BSONObj& lookupSpec) {
        return DocumentSourceLookUp::createFromBson(lookupSpec.firstElement(), _expCtx);
    }

    boost::intrusive_ptr<DocumentSource> graphLookupStage(const BSONObj& graphLookupSpec) {
        return DocumentSourceGraphLookUp::createFromBson(graphLookupSpec.firstElement(), _expCtx);
    }

    boost::intrusive_ptr<DocumentSource> sortStage(const std::string& sortStr) {
        return DocumentSourceSort::create(_expCtx, fromjson(sortStr));
    }

    boost::intrusive_ptr<DocumentSourceCanSwapWithSort> swappableStage() {
        return DocumentSourceCanSwapWithSort::create(_expCtx);
    }
    // End of functions that are used for making parts of the sources for making a pipeline.

    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const Pipeline::SourceContainer& sources) {
        return Pipeline::create(sources, _expCtx);
    }

    sharded_agg_helpers::SplitPipeline makeAndSplitPipeline(
        const Pipeline::SourceContainer& sources) {
        auto pipeline = makePipeline(sources);
        return sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));
    }

    void verifyPipelineForDeferredMergeSortTest(
        const sharded_agg_helpers::SplitPipeline& splitPipeline,
        size_t shardsPipelineSize,
        size_t mergePipelineSize,
        const BSONObj& shardCursorSortSpec) {
        // Verify that we've split the pipeline at the SplitPipeline stage, not on the deferred.
        ASSERT_EQ(splitPipeline.shardsPipeline->getSources().size(), shardsPipelineSize);
        ASSERT_EQ(splitPipeline.mergePipeline->getSources().size(), mergePipelineSize);

        // Verify the sort is correct.
        ASSERT(splitPipeline.shardCursorsSortSpec);
        ASSERT_BSONOBJ_EQ(splitPipeline.shardCursorsSortSpec.value(), shardCursorSortSpec);
    }

    void trackPipelineRenames(const std::unique_ptr<Pipeline, PipelineDeleter>& pipeline,
                              const mongo::OrderedPathSet& pathsOfInterest,
                              Tracking dir) {
        const auto& stages = pipeline->getSources();
        auto renames = (dir == Tracking::forwards)
            ? semantic_analysis::renamedPaths(stages.cbegin(), stages.cend(), pathsOfInterest)
            : semantic_analysis::renamedPaths(stages.crbegin(), stages.crend(), pathsOfInterest);

        ASSERT(renames.has_value());
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), pathsOfInterest.size());
        for (const auto& p : pathsOfInterest) {
            ASSERT_EQ(nameMap[p], p);
        }
    }

    void trackPipelineRenamesOnEmptyRange(
        const std::unique_ptr<Pipeline, PipelineDeleter>& pipeline,
        const mongo::OrderedPathSet& pathsOfInterest,
        Tracking dir) {
        const auto& stages = pipeline->getSources();
        auto renames = (dir == Tracking::forwards)
            ? semantic_analysis::renamedPaths(stages.cbegin(), stages.cbegin(), pathsOfInterest)
            : semantic_analysis::renamedPaths(stages.crbegin(), stages.crbegin(), pathsOfInterest);

        ASSERT(renames.has_value());
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), pathsOfInterest.size());
        for (const auto& p : pathsOfInterest) {
            ASSERT_EQ(nameMap[p], p);
        }
    }


private:
    const unittest::TempDir _tempDir{"AggregationContextFixture"};

    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

// A custom-deleter which disposes a DocumentSource when it goes out of scope.
struct DocumentSourceDeleter {
    void operator()(DocumentSource* docSource) {
        docSource->dispose();
        delete docSource;
    }
};

namespace {
// A utility function to convert pipeline (a vector of BSONObj) to a string. Helpful for debugging.
std::string to_string(const std::vector<BSONObj>& objs) {
    std::stringstream sstrm;
    sstrm << "[" << std::endl;
    for (const auto& obj : objs) {
        sstrm << obj.toString() << "," << std::endl;
    }
    sstrm << "]" << std::endl;
    return sstrm.str();
}
}  // namespace
}  // namespace mongo
