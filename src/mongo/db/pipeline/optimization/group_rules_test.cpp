// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

class GroupRulesTest : public AggregationContextFixture {
protected:
    void setUp() override {
        AggregationContextFixture::setUp();
        getExpCtx()->setPathArraynessForNss(getExpCtx()->getNamespaceString(), _pathArrayness);
    }

    void addIndexedPath(const std::string& path, MultikeyComponents multikey = {}) {
        _pathArrayness->addPath(FieldPath(path), multikey, /*isFullRebuild=*/true);
    }

    std::unique_ptr<Pipeline> makePipeline(const std::vector<std::string>& stages) {
        std::vector<BSONObj> bsonStages;
        for (auto&& stage : stages) {
            bsonStages.push_back(fromjson(stage));
        }
        return pipeline_factory::makePipeline(
            bsonStages, getExpCtx(), pipeline_factory::kOptionsMinimal);
    }

    static DocumentSourceGroup& lastGroup(Pipeline& pipeline) {
        return checked_cast<DocumentSourceGroup&>(*pipeline.getSources().back());
    }

    OrderedPathSet optimizeAndGetNonArrayIdFields(const std::vector<std::string>& stages) {
        auto pipeline = makePipeline(stages);
        pipeline_optimization::optimizePipeline(*pipeline);
        return lastGroup(*pipeline).getNonArrayIdFields();
    }

    std::vector<std::string> recordedNonArrayPaths() {
        std::vector<std::string> paths;
        for (auto&& path : getExpCtx()->nonArrayPathsForNss(getExpCtx()->getNamespaceString())) {
            paths.push_back(path.fullPath());
        }
        return paths;
    }

private:
    std::shared_ptr<PathArrayness> _pathArrayness = std::make_shared<PathArrayness>();
    unittest::ServerParameterGuard _featureFlag{"featureFlagSbeStreamingGroup", true};
};

TEST_F(GroupRulesTest, TopLevelKey) {
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"}), OrderedPathSet({"_id"}));
}

TEST_F(GroupRulesTest, DottedKey) {
    addIndexedPath("a.b");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a.b'}}"}), OrderedPathSet({"_id"}));
}

TEST_F(GroupRulesTest, DottedKeyUnderMultikeyPrefix) {
    addIndexedPath("a.b", MultikeyComponents{0U});
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a.b'}}"}), OrderedPathSet{});
}

TEST_F(GroupRulesTest, KeyWithoutArraynessInformation) {
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"}), OrderedPathSet{});
}

TEST_F(GroupRulesTest, OnlyFieldPathKeysOfObjectKey) {
    addIndexedPath("a");
    addIndexedPath("b.c");
    addIndexedPath("s");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields(
                  {"{$group: {_id: {x: '$a', y: '$b.c', z: {$toUpper: '$s'}, w: '$$ROOT', "
                   "c: {$literal: 1}}}}"}),
              OrderedPathSet({"_id.x", "_id.y"}));
}

TEST_F(GroupRulesTest, KeyRenamedByEarlierStage) {
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$set: {k: '$a'}}", "{$group: {_id: '$k'}}"}),
              OrderedPathSet({"_id"}));
}

TEST_F(GroupRulesTest, KeyUnwoundByEarlierStage) {
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$unwind: '$a'}", "{$group: {_id: '$a'}}"}),
              OrderedPathSet{});
}

TEST_F(GroupRulesTest, RecordsPathsForYieldRecheck) {
    addIndexedPath("a");
    optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"});
    ASSERT_EQ(recordedNonArrayPaths(), std::vector<std::string>{"a"});
}

TEST_F(GroupRulesTest, FeatureFlagOff) {
    unittest::ServerParameterGuard featureFlag("featureFlagSbeStreamingGroup", false);
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"}), OrderedPathSet{});
    ASSERT_EQ(recordedNonArrayPaths(), std::vector<std::string>{});
}

TEST_F(GroupRulesTest, ForceClassicEngine) {
    QueryKnobGuardForTest knob(getOpCtx(), "internalQueryFrameworkControl", "forceClassicEngine");
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"}), OrderedPathSet{});
    ASSERT_EQ(recordedNonArrayPaths(), std::vector<std::string>{});
}

TEST_F(GroupRulesTest, GroupPushdownDisabled) {
    QueryKnobGuardForTest knob(
        getOpCtx(), "internalQuerySlotBasedExecutionDisableGroupPushdown", true);
    addIndexedPath("a");
    ASSERT_EQ(optimizeAndGetNonArrayIdFields({"{$group: {_id: '$a'}}"}), OrderedPathSet{});
}

TEST_F(GroupRulesTest, MergingGroup) {
    addIndexedPath("a");
    auto pipeline = makePipeline({"{$group: {_id: '$a'}}"});
    lastGroup(*pipeline).getGroupProcessor()->setDoingMerge(true);
    pipeline_optimization::optimizePipeline(*pipeline);
    ASSERT_EQ(lastGroup(*pipeline).getNonArrayIdFields(), OrderedPathSet{});
}

TEST_F(GroupRulesTest, CloneDropsAnnotation) {
    addIndexedPath("a");
    auto pipeline = makePipeline({"{$group: {_id: '$a'}}"});
    pipeline_optimization::optimizePipeline(*pipeline);
    ASSERT_EQ(lastGroup(*pipeline).getNonArrayIdFields(), OrderedPathSet({"_id"}));

    auto clone = lastGroup(*pipeline).clone(getExpCtx());
    ASSERT_EQ(checked_cast<DocumentSourceGroup&>(*clone).getNonArrayIdFields(), OrderedPathSet{});
}

}  // namespace
}  // namespace mongo
