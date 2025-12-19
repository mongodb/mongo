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

#include "mongo/bson/json.h"
#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>

namespace mongo::extension::host {

class LoadNativeVectorSearchTest : public unittest::Test {
protected:
    LoadNativeVectorSearchTest() : expCtx(make_intrusive<ExpressionContextForTest>()) {}

    static inline const std::string kNativeVectorSearchStageName = "$nativeVectorSearch";
    static inline const std::string kNativeVectorSearchLibExtensionPath =
        "libnative_vector_search_mongo_extension.so";
    static inline const std::string kMetricsStageName = "$vectorSearchMetrics";

    void setUp() override {
        ASSERT_DOES_NOT_THROW(
            ExtensionLoader::load("nativeVectorSearch", makeNativeVectorSearchConfig()));
    }

    void tearDown() override {
        LiteParsedDocumentSource::unregisterParser_forTest(kMetricsStageName);
        LiteParsedDocumentSource::unregisterParser_forTest(kNativeVectorSearchStageName);
        ExtensionLoader::unload_forTest("nativeVectorSearch");
    }

    ExtensionConfig makeNativeVectorSearchConfig() {
        return test_util::makeEmptyExtensionConfig(kNativeVectorSearchLibExtensionPath);
    }

    // Test helpers
    static BSONObj makeNativeVectorSearchSpec(bool includeFilter = false,
                                              std::string metric = "cosine",
                                              bool normalizeScore = false,
                                              boost::optional<int> numCandidates = boost::none) {
        BSONObjBuilder specBuilder;
        specBuilder.append("path", "embedding");

        {
            BSONArrayBuilder arr;
            arr.append(1.0);
            arr.append(2.0);
            arr.append(3.0);
            specBuilder.appendArray("queryVector", arr.arr());
        }

        specBuilder.append("limit", 5);
        specBuilder.append("metric", metric);

        if (normalizeScore) {
            specBuilder.append("normalizeScore", true);
        }

        if (includeFilter) {
            specBuilder.append("filter", BSON("x" << 1));
        }

        if (numCandidates) {
            specBuilder.append("numCandidates", *numCandidates);
        }

        return BSON(kNativeVectorSearchStageName << specBuilder.obj());
    }

    static std::vector<BSONObj> desugarAndSerialize(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& stageSpec) {
        std::vector<BSONObj> spec{stageSpec};
        auto pipe = Pipeline::parse(spec, expCtx);
        ASSERT_TRUE(pipe);
        Desugarer(pipe.get())();
        return pipe->serializeToBson();
    }

    static void expectStageEq(const std::vector<BSONObj>& stages,
                              size_t index,
                              const char* expectedJson) {
        ASSERT_LT(index, stages.size());
        ASSERT_BSONOBJ_EQ(stages[index], fromjson(expectedJson));
    }

    static void expectLiteParsedNames(const NamespaceString& nss,
                                      const BSONObj& stageSpec,
                                      std::initializer_list<const StringData> names) {
        auto liteParsed = LiteParsedDocumentSource::parse(nss, stageSpec);
        auto* lpExpanded =
            dynamic_cast<DocumentSourceExtension::LiteParsedExpandable*>(liteParsed.get());
        ASSERT_TRUE(lpExpanded);
        const auto& expanded = lpExpanded->getExpandedPipeline();
        ASSERT_EQ(expanded.size(), names.size());

        size_t i = 0;
        for (const auto& expected : names) {
            auto* lp = expanded[i].get();
            ASSERT_TRUE(lp);
            ASSERT_EQ(lp->getParseTimeName(), expected);
            ++i;
        }
    }

    // Common fragments
    static constexpr const char* kSortJson =
        R"JSON({ $sort:  { $computed0: { $meta: "vectorSearchScore" } } })JSON";
    static constexpr const char* kLimitJson = R"JSON({ $limit: 5 })JSON";

    boost::intrusive_ptr<ExpressionContext> expCtx;

    static inline NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "load_native_vector_search_extension_test");

private:
    RAIIServerParameterControllerForTest _extensionsAPIController{"featureFlagExtensionsAPI", true};
    RAIIServerParameterControllerForTest _vecSimilarityExprController{
        "featureFlagVectorSimilarityExpressions", true};
};

// Tests successful desugar extension loading and verifies stage registration works in pipelines.
// The libnative_vector_search_mongo_extension.so adds a "$nativeVectorSearch" stage for testing.
TEST_F(LoadNativeVectorSearchTest, ExtensionRegistration) {
    auto stageSpec = makeNativeVectorSearchSpec(true /*filter*/);

    auto sourceList = DocumentSource::parse(expCtx, stageSpec);
    ASSERT_EQ(sourceList.size(), 1U);

    auto* extensionStage = dynamic_cast<DocumentSourceExtension*>(sourceList.front().get());
    ASSERT_TRUE(extensionStage);
    ASSERT_EQ(std::string(extensionStage->getSourceName()), kNativeVectorSearchStageName);

    // Verify the stage can be used in a pipeline with other existing stages.
    auto parsedPipeline = Pipeline::parse({stageSpec, BSON("$skip" << 1)}, expCtx);
    ASSERT(parsedPipeline);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto* firstStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    ASSERT(firstStage);
    ASSERT_EQUALS(std::string(firstStage->getSourceName()), kNativeVectorSearchStageName);
}

TEST_F(LoadNativeVectorSearchTest, LiteParsedExpandsWithFilter) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ true);
    expectLiteParsedNames(nss,
                          spec,
                          {
                              kMetricsStageName,
                              DocumentSourceMatch::kStageName,
                              DocumentSourceSetMetadata::kStageName,
                              DocumentSourceSort::kStageName,
                              DocumentSourceLimit::kStageName,
                          });
}

TEST_F(LoadNativeVectorSearchTest, LiteParsedExpandsWithoutFilter) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ false);
    expectLiteParsedNames(nss,
                          spec,
                          {
                              kMetricsStageName,
                              DocumentSourceSetMetadata::kStageName,
                              DocumentSourceSort::kStageName,
                              DocumentSourceLimit::kStageName,
                          });
}

TEST_F(LoadNativeVectorSearchTest, FullParseExpandsWithFilter) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ true);
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 5U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "cosine" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $match: { x: 1 } })JSON");
    expectStageEq(stages, 2, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityCosine: {
        vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
        score: false } } } }
        )JSON");
    expectStageEq(stages, 3, kSortJson);
    expectStageEq(stages, 4, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, FullParseExpandsWithoutFilter) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ false);  // default = "cosine"
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "cosine" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityCosine: {
        vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
        score: false } } } }
        )JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, CosineNormalizedSerializesAsExpected) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ false, "cosine", /*normalize*/ true);
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);
    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "cosine" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityCosine: {
      vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
      score: true } } } })JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, DotProductNoNormalizeSerializesAsExpected) {
    auto spec = makeNativeVectorSearchSpec(/*filter*/ false, /*metric*/ "dotProduct");
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "dotProduct" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityDotProduct: {
        vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
        score: false } } } }
        )JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, DotProductNormalizedSerializesAsExpected) {
    auto spec = makeNativeVectorSearchSpec(false, "dotProduct", /*normalize*/ true);
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "dotProduct" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityDotProduct: {
      vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
      score: true } } } })JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, NativeVectorSearchEuclideanNoNormalizeUsesMultiplyNegation) {
    auto spec =
        makeNativeVectorSearchSpec(/*filter*/ false, /*metric*/ "euclidean", /*normalize*/ false);
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "euclidean" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $multiply: [
        { $const: -1 },
        { $similarityEuclidean: {
            vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
            score: false } }
        ] } } }
        )JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, NativeVectorSearchEuclideanNormalizedSerializesAsExpected) {
    auto spec =
        makeNativeVectorSearchSpec(/*filter*/ false, /*metric*/ "euclidean", /*normalize*/ true);
    auto stages = desugarAndSerialize(expCtx, spec);
    ASSERT_EQ(stages.size(), 4U);

    expectStageEq(stages, 0, R"JSON({ $vectorSearchMetrics: { metric: "euclidean" } })JSON");
    expectStageEq(stages, 1, R"JSON({ $setMetadata: { vectorSearchScore: { $similarityEuclidean: {
        vectors: [ [ { $const: 1.0 }, { $const: 2.0 }, { $const: 3.0 } ], "$embedding" ],
        score: true } } } }
        )JSON");
    expectStageEq(stages, 2, kSortJson);
    expectStageEq(stages, 3, kLimitJson);
}

TEST_F(LoadNativeVectorSearchTest, NumCandidatesDoesNotAffectExpansionOrSerialization) {
    auto base = makeNativeVectorSearchSpec(
        /*filter*/ false, "cosine", /*normalize*/ false, /*numCandidates*/ boost::none);
    auto with = makeNativeVectorSearchSpec(
        /*filter*/ false, "cosine", /*normalize*/ false, /*numCandidates*/ 5);

    auto a = desugarAndSerialize(expCtx, base);
    auto b = desugarAndSerialize(expCtx, with);

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_BSONOBJ_EQ(a[i], b[i]);
    }
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidPath) {
    auto missing = BSON(kNativeVectorSearchStageName << BSON(
                            // "path" missing
                            "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0) << "limit" << 5
                                          << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, missing), AssertionException, 10956503);

    auto nonString = BSON(kNativeVectorSearchStageName
                          << BSON("path" << 123 << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                                         << "limit" << 5 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonString), AssertionException, 10956503);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidQueryVector) {
    auto missing = BSON(kNativeVectorSearchStageName << BSON("path" << "embedding" <<
                                                             // "queryVector" missing
                                                             "limit" << 5 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, missing), AssertionException, 10956504);

    auto nonArray =
        BSON(kNativeVectorSearchStageName << BSON("path" << "embedding" << "queryVector" << 1
                                                         << "limit" << 5 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonArray), AssertionException, 10956504);

    auto nonNumericValue =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << "str" << 3.0)
                            << "limit" << 5 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(
        DocumentSource::parse(expCtx, nonNumericValue), AssertionException, 10956505);

    auto empty = BSON(kNativeVectorSearchStageName
                      << BSON("path" << "embedding" << "queryVector" << BSONArray() << "limit" << 5
                                     << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, empty), AssertionException, 10956506);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidMetric) {
    auto missing = BSON(kNativeVectorSearchStageName
                        << BSON("path" << "embedding" << "queryVector"
                                       << BSON_ARRAY(1.0 << 2.0 << 3.0) << "limit" << 5
                                // "metric" missing
                                ));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, missing), AssertionException, 10956507);

    auto nonString =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << 5 << "metric" << 123));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonString), AssertionException, 10956507);

    auto badValue = BSON(kNativeVectorSearchStageName << BSON(
                             "path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                                    << "limit" << 5 << "metric" << "manhattan"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, badValue), AssertionException, 10956508);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidLimit) {
    auto missing = BSON(kNativeVectorSearchStageName
                        << BSON("path" << "embedding" << "queryVector"
                                       << BSON_ARRAY(1.0 << 2.0 << 3.0) << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, missing), AssertionException, 10956509);

    auto nonNumeric =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << "five" << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonNumeric), AssertionException, 10956510);

    auto zero = BSON(kNativeVectorSearchStageName
                     << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                                    << "limit" << 0 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, zero), AssertionException, 10956510);

    auto negative = BSON(kNativeVectorSearchStageName << BSON(
                             "path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                                    << "limit" << -1 << "metric" << "cosine"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, negative), AssertionException, 10956510);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidNormalizeScore) {
    auto nonBool =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << 5 << "metric" << "cosine" << "normalizeScore" << 123));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonBool), AssertionException, 10956511);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidFilter) {
    auto nonObject =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << 5 << "metric" << "cosine" << "filter" << 1));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonObject), AssertionException, 10956512);
}

TEST_F(LoadNativeVectorSearchTest, RejectsInvalidNumCandidates) {
    auto nonNumeric =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << 5 << "metric" << "cosine" << "numCandidates" << "five"));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, nonNumeric), AssertionException, 10956513);

    auto zero = BSON(kNativeVectorSearchStageName << BSON(
                         "path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                                << "limit" << 5 << "metric" << "cosine" << "numCandidates" << 0));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, zero), AssertionException, 10956514);

    auto negative =
        BSON(kNativeVectorSearchStageName
             << BSON("path" << "embedding" << "queryVector" << BSON_ARRAY(1.0 << 2.0 << 3.0)
                            << "limit" << 5 << "metric" << "cosine" << "numCandidates" << -10));
    ASSERT_THROWS_CODE(DocumentSource::parse(expCtx, negative), AssertionException, 10956514);
}
}  // namespace mongo::extension::host
