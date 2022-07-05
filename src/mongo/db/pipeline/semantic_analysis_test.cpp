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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using namespace semantic_analysis;

using SemanticAnalysisRenamedPaths = AggregationContextFixture;

class RenamesAToB : public DocumentSourceTestOptimizations {
public:
    RenamesAToB(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    GetModPathsReturn getModifiedPaths() const final {
        // Pretend this stage simply renames the "a" field to be "b", leaving the value of "a" the
        // same. This would be the equivalent of an {$addFields: {b: "$a"}}.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {{"b", "a"}}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, DoesReturnSimpleRenameFromFiniteSetRename) {
    RenamesAToB renamesAToB(getExpCtx());
    {
        auto renames = renamedPaths({"a"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "b");
    }
    {
        auto renames = renamedPaths({"b"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["b"], "a");
    }
}

TEST_F(SemanticAnalysisRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromFiniteSetRename) {
    RenamesAToB renamesAToB(getExpCtx());
    {
        auto renames = renamedPaths({"c"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "c");
    }
    {
        auto renames = renamedPaths({"c"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "c");
    }
    {
        auto renames = renamedPaths({"a"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "b");
    }
    {
        auto renames = renamedPaths({"b"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["b"], "a");
    }
    {
        auto renames = renamedPaths({"e", "f", "g"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 3UL);
        ASSERT_EQ(map["e"], "e");
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
    {
        auto renames = renamedPaths({"e", "f", "g"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 3UL);
        ASSERT_EQ(map["e"], "e");
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
}

class RenameCToDPreserveEFG : public DocumentSourceTestOptimizations {
public:
    RenameCToDPreserveEFG(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllExcept, OrderedPathSet{"e", "f", "g"}, {{"d", "c"}}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, DoesReturnSimpleRenameFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG(getExpCtx());
    {
        auto renames = renamedPaths({"c"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "d");
    }
    {
        auto renames = renamedPaths({"d"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["d"], "c");
    }
}

TEST_F(SemanticAnalysisRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG(getExpCtx());
    {
        auto renames = renamedPaths({"e"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "e");
    }
    {
        auto renames = renamedPaths({"e"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "e");
    }
    {
        auto renames = renamedPaths({"f", "g"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
    {
        auto renames = renamedPaths({"f", "g"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
}

class RenameCDotDToEPreserveFDotG : public DocumentSourceTestOptimizations {
public:
    RenameCDotDToEPreserveFDotG(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllExcept, OrderedPathSet{"f.g"}, {{"e", "c.d"}}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, DoesReturnRenameToDottedFieldFromAllExceptRename) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG(getExpCtx());
    {
        auto renames = renamedPaths({"c.d"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c.d"], "e");
    }
    {
        auto renames = renamedPaths({"e"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "c.d");
    }
    {
        auto renames =
            renamedPaths({"c.d.x", "c.d.y"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.d.x"], "e.x");
        ASSERT_EQ(map["c.d.y"], "e.y");
    }
    {
        auto renames =
            renamedPaths({"e.x", "e.y"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["e.x"], "c.d.x");
        ASSERT_EQ(map["e.y"], "c.d.y");
    }
}

TEST_F(SemanticAnalysisRenamedPaths,
       DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsModifiedFromAllExcept) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG(getExpCtx());
    {
        auto renames = renamedPaths({"f"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"f"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // This is the exception, the only path that is not modified.
        auto renames = renamedPaths({"f.g"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["f.g"], "f.g");
    }
    {
        // This is the exception, the only path that is not modified.
        auto renames = renamedPaths({"f.g"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["f.g"], "f.g");
    }
    {
        // We know "f.g" is preserved, so it follows that a subpath of that path is also preserved.
        auto renames = renamedPaths(
            {"f.g.x", "f.g.xyz.foobarbaz"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f.g.x"], "f.g.x");
        ASSERT_EQ(map["f.g.xyz.foobarbaz"], "f.g.xyz.foobarbaz");
    }
    {
        auto renames = renamedPaths(
            {"f.g.x", "f.g.xyz.foobarbaz"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f.g.x"], "f.g.x");
        ASSERT_EQ(map["f.g.xyz.foobarbaz"], "f.g.xyz.foobarbaz");
    }
    {
        // This shares a prefix with the unmodified path, but should not be reported as unmodified.
        auto renames = renamedPaths({"f.x"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

class RenameAToXDotYModifyCDotD : public DocumentSourceTestOptimizations {
public:
    RenameAToXDotYModifyCDotD(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{"c.d"}, {{"x.y", "a"}}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, DoesReturnRenameToDottedFieldFromFiniteSetRename) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD(getExpCtx());
    {
        auto renames = renamedPaths({"a"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "x.y");
    }
    {
        auto renames = renamedPaths({"x.y"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["x.y"], "a");
    }
    {
        auto renames =
            renamedPaths({"a.z", "a.a.b.c"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["a.z"], "x.y.z");
        ASSERT_EQ(map["a.a.b.c"], "x.y.a.b.c");
    }
    {
        auto renames =
            renamedPaths({"x.y.z", "x.y.a.b.c"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["x.y.z"], "a.z");
        ASSERT_EQ(map["x.y.a.b.c"], "a.a.b.c");
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsPartOfModifiedSet) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD(getExpCtx());
    {
        auto renames = renamedPaths({"c"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d.e"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d.e"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths({"c.not_d", "c.decoy"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.not_d"], "c.not_d");
        ASSERT_EQ(map["c.decoy"], "c.decoy");
    }
    {
        auto renames =
            renamedPaths({"c.not_d", "c.decoy"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.not_d"], "c.not_d");
        ASSERT_EQ(map["c.decoy"], "c.decoy");
    }
}

class ModifiesAllPaths : public DocumentSourceTestOptimizations {
public:
    ModifiesAllPaths(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, ReturnsNoneWhenAllPathsAreModified) {
    ModifiesAllPaths modifiesAllPaths(getExpCtx());
    {
        auto renames = renamedPaths({"a"}, modifiesAllPaths, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modifiesAllPaths, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a"}, modifiesAllPaths, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modifiesAllPaths, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

class ModificationsUnknown : public DocumentSourceTestOptimizations {
public:
    ModificationsUnknown(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kNotSupported, OrderedPathSet{}, {}};
    }
};

TEST_F(SemanticAnalysisRenamedPaths, ReturnsNoneWhenModificationsAreNotKnown) {
    ModificationsUnknown modificationsUnknown(getExpCtx());
    {
        auto renames = renamedPaths({"a"}, modificationsUnknown, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modificationsUnknown, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a"}, modificationsUnknown, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modificationsUnknown, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DetectsSimpleReplaceRootPattern) {
    auto pipeline = Pipeline::parse(
        {fromjson("{$replaceWith: {nested: '$$ROOT'}}"), fromjson("{$replaceWith: '$nested'}")},
        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"b"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DetectsReplaceRootPatternAllowsIntermediateStages) {
    auto pipeline =
        Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                         fromjson("{$set: {bigEnough: {$gte: [{$bsonSize: '$nested'}, 300]}}}"),
                         fromjson("{$match: {bigEnough: true}}"),
                         fromjson("{$replaceWith: '$nested'}")},
                        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"b"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, AdditionalStageValidatorCallbackPassed) {
    auto pipeline =
        Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                         fromjson("{$set: {bigEnough: {$gte: [{$bsonSize: '$nested'}, 300]}}}"),
                         fromjson("{$match: {bigEnough: true}}"),
                         fromjson("{$replaceWith: '$nested'}")},
                        getExpCtx());
    std::function<bool(DocumentSource*)> callback = [](DocumentSource* stage) {
        return !static_cast<bool>(stage->distributedPlanLogic());
    };
    {
        auto renames = renamedPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"a"}, callback);
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"b"}, callback);
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths(
            pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"}, callback);
        ASSERT_TRUE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, AdditionalStageValidatorCallbackNotPassed) {
    auto pipeline =
        Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                         fromjson("{$set: {bigEnough: {$gte: [{$bsonSize: '$nested'}, 300]}}}"),
                         fromjson("{$match: {bigEnough: true}}"),
                         fromjson("{$sort: {x: 1}}"),
                         fromjson("{$replaceWith: '$nested'}")},
                        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    std::function<bool(DocumentSource*)> callback = [](DocumentSource* stage) {
        return !static_cast<bool>(stage->distributedPlanLogic());
    };
    {
        auto renames = renamedPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"a"}, callback);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths(
            pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"}, callback);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DetectsReplaceRootPatternDisallowsIntermediateModification) {
    auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                     fromjson("{$set: {'nested.field': 'anyNewValue'}}"),
                                     fromjson("{$replaceWith: '$nested'}")},
                                    getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DoesNotDetectFalseReplaceRootIfTypoed) {
    auto pipeline = Pipeline::parse(
        {fromjson("{$replaceWith: {nested: '$$ROOT'}}"), fromjson("{$replaceWith: '$nestedTypo'}")},
        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DetectsReplaceRootPatternIfCurrentInsteadOfROOT) {
    auto pipeline = Pipeline::parse(
        {fromjson("{$replaceWith: {nested: '$$CURRENT'}}"), fromjson("{$replaceWith: '$nested'}")},
        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_TRUE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DoesNotDetectFalseReplaceRootIfNoROOT) {
    auto pipeline = Pipeline::parse(
        {fromjson("{$replaceWith: {nested: '$subObj'}}"), fromjson("{$replaceWith: '$nested'}")},
        getExpCtx());
    {
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

TEST_F(SemanticAnalysisRenamedPaths, DoesNotDetectFalseReplaceRootIfTargetPathIsRenamed) {

    {
        auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                         fromjson("{$unset : 'nested'}"),
                                         fromjson("{$replaceWith: '$nested'}")},
                                        getExpCtx());
        auto renames =
            renamedPaths(pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                         fromjson("{$set : {nested: '$somethingElese'}}"),
                                         fromjson("{$replaceWith: '$nested'}")},
                                        getExpCtx());
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // This case could someday work - we leave it as a future improvement.
        auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                         fromjson("{$set : {somethingElse: '$nested'}}"),
                                         fromjson("{$replaceWith: '$somethingElse'}")},
                                        getExpCtx());
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // This is a tricky one. The pattern does exist, but it's doubly nested and only unnested
        // once.
        auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                         fromjson("{$replaceWith: {doubleNested: '$nested'}}"),
                                         fromjson("{$replaceWith: '$doubleNested'}")},
                                        getExpCtx());
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // Similar to above but double nested then double unnested. We could someday make this work,
        // but leave it for a future improvement.
        auto pipeline = Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                                         fromjson("{$replaceWith: {doubleNested: '$nested'}}"),
                                         fromjson("{$replaceWith: '$doubleNested'}"),
                                         fromjson("{$replaceWith: '$nested'}")},
                                        getExpCtx());
        auto renames =
            renamedPaths(pipeline->getSources().rbegin(), pipeline->getSources().rend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

using SemanticAnalysisFindLongestViablePrefix = AggregationContextFixture;
TEST_F(SemanticAnalysisFindLongestViablePrefix, AllowsReplaceRootPattern) {
    auto pipeline =
        Pipeline::parse({fromjson("{$replaceWith: {nested: '$$ROOT'}}"),
                         fromjson("{$set: {bigEnough: {$gte: [{$bsonSize: '$nested'}, 300]}}}"),
                         fromjson("{$match: {bigEnough: true}}"),
                         fromjson("{$replaceWith: '$nested'}")},
                        getExpCtx());
    auto [itr, renames] = findLongestViablePrefixPreservingPaths(
        pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
    ASSERT(itr == pipeline->getSources().end());
}

TEST_F(SemanticAnalysisFindLongestViablePrefix, FindsPrefixWithoutReplaceRoot) {
    auto pipeline = Pipeline::parse({fromjson("{$match: {testing: true}}"),
                                     fromjson("{$unset: 'unset'}"),
                                     fromjson("{$set: {x: '$y'}}")},
                                    getExpCtx());
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"a"});
        ASSERT(itr == pipeline->getSources().end());
    }
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"unset"});
        ASSERT(itr == std::next(pipeline->getSources().begin()));
    }
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"y"});
        ASSERT(itr == pipeline->getSources().end());
        ASSERT(renames["y"] == "x");
    }
    {
        // TODO (SERVER-55815): "x" should be considered modified in the $set stage.
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"x"});
        ASSERT(itr == pipeline->getSources().end());
        ASSERT(renames["x"] == "x");
    }
}

TEST_F(SemanticAnalysisFindLongestViablePrefix, FindsLastPossibleStageWithCallback) {
    auto pipeline = Pipeline::parse({fromjson("{$match: {testing: true}}"),
                                     fromjson("{$unset: 'unset'}"),
                                     fromjson("{$sort: {y: 1}}"),
                                     fromjson("{$set: {x: '$y'}}")},
                                    getExpCtx());
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"y"});
        ASSERT(itr == pipeline->getSources().end());
        ASSERT(renames["y"] == "x");
    }
    std::function<bool(DocumentSource*)> callback = [](DocumentSource* stage) {
        return !static_cast<bool>(stage->distributedPlanLogic());
    };
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"y"}, callback);
        ASSERT(itr == std::prev(std::prev(pipeline->getSources().end())));
        ASSERT(renames["y"] == "y");
    }
}

TEST_F(SemanticAnalysisFindLongestViablePrefix, CorrectlyAnswersReshardingUseCase) {
    auto expCtx = getExpCtx();
    auto lookupNss = NamespaceString{"config.cache.chunks.test"};
    expCtx->setResolvedNamespace(lookupNss, ExpressionContext::ResolvedNamespace{lookupNss, {}});
    auto pipeline =
        Pipeline::parse({fromjson("{$replaceWith: {original: '$$ROOT'}}"),
                         fromjson("{$lookup: {from: {db: 'config', coll: 'cache.chunks.test'}, "
                                  "pipeline: [], as: 'intersectingChunk'}}"),
                         fromjson("{$match: {intersectingChunk: {$ne: []}}}"),
                         fromjson("{$replaceWith: '$original'}")},
                        getExpCtx());
    std::function<bool(DocumentSource*)> callback = [](DocumentSource* stage) {
        return !static_cast<bool>(stage->distributedPlanLogic());
    };
    {
        auto [itr, renames] = findLongestViablePrefixPreservingPaths(
            pipeline->getSources().begin(), pipeline->getSources().end(), {"_id"}, callback);
        ASSERT(itr == pipeline->getSources().end());
        ASSERT(renames["_id"] == "_id");
    }
}

}  // namespace
}  // namespace mongo
