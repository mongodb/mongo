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


#include "mongo/db/pipeline/document_source_match.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <iterator>
#include <list>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using std::string;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceMatchTest = AggregationContextFixture;


TEST_F(DocumentSourceMatchTest, RedactSafePortion) {
    auto expCtx = getExpCtx();
    auto assertExpectedRedactSafePortion = [&expCtx](string input, string safePortion) {
        try {
            auto match = DocumentSourceMatch::create(fromjson(input), expCtx);
            ASSERT_BSONOBJ_EQ(match->redactSafePortion(), fromjson(safePortion));
        } catch (...) {
            LOGV2(20899, "Problem with redactSafePortion() of: {input}", "input"_attr = input);
            throw;
        }
    };

    // Empty
    assertExpectedRedactSafePortion("{}", "{}");

    // Basic allowed things
    assertExpectedRedactSafePortion("{a:1}", "{a:1}");

    assertExpectedRedactSafePortion("{a:'asdf'}", "{a:'asdf'}");

    assertExpectedRedactSafePortion("{a:/asdf/i}", "{a:/asdf/i}");

    assertExpectedRedactSafePortion("{a: {$regex: 'adsf'}}", "{a: {$regex: 'adsf'}}");

    assertExpectedRedactSafePortion("{a: {$regex: 'adsf', $options: 'i'}}",
                                    "{a: {$regex: 'adsf', $options: 'i'}}");

    assertExpectedRedactSafePortion("{a: {$mod: [1, 0]}}", "{a: {$mod: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$type: 1}}", "{a: {$type: 1}}");

    // Basic disallowed things
    assertExpectedRedactSafePortion("{a: null}", "{}");

    assertExpectedRedactSafePortion("{a: {}}", "{}");

    assertExpectedRedactSafePortion("{a: []}", "{}");

    assertExpectedRedactSafePortion("{'a.0': 1}", "{}");

    assertExpectedRedactSafePortion("{'a.0.b': 1}", "{}");

    assertExpectedRedactSafePortion("{a: {$ne: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$nin: [1, 2, 3]}}", "{}");

    assertExpectedRedactSafePortion("{a: {$exists: true}}",
                                    "{}");  // could be allowed but currently isn't

    assertExpectedRedactSafePortion("{a: {$exists: false}}", "{}");  // can never be allowed

    assertExpectedRedactSafePortion("{a: {$size: 1}}", "{}");

    assertExpectedRedactSafePortion("{$nor: [{a:1}]}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMinItems: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMaxItems: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaUniqueItems: true}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMinLength: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMaxLength: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaFmod: [4.5, 2.3]}}", "{}");

    assertExpectedRedactSafePortion(
        "{a: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$gt: 0}}}}}",
        "{}");

    assertExpectedRedactSafePortion(
        "{a: {$_internalSchemaAllElemMatchFromIndex: [3, {a: {$lt: 4}}]}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaType: 2}}", "{}");

    // In some cases, $_internalExprEq could be redact-safe (just like a regular $eq match
    // expression), but this optimization is not yet implemented.
    assertExpectedRedactSafePortion("{a: {$_internalExprEq: 2}}", "{}");

    // Combinations
    assertExpectedRedactSafePortion("{a:1, b: 'asdf'}", "{a:1, b: 'asdf'}");

    assertExpectedRedactSafePortion("{a:1, b: null}", "{a:1}");

    assertExpectedRedactSafePortion("{a:null, b: null}", "{}");

    // $elemMatch

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b: 1}}}", "{a: {$elemMatch: {b: 1}}}");

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b:null}}}", "{}");

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b:null, c:1}}}",
                                    "{a: {$elemMatch: {c: 1}}}");

    // explicit $and
    assertExpectedRedactSafePortion("{$and:[{a: 1}]}", "{$and:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: 1}, {b: null}]}", "{$and:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: 1}, {b: null, c:1}]}", "{$and:[{a: 1}, {c:1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: null}, {b: null}]}", "{}");

    // explicit $or
    assertExpectedRedactSafePortion("{$or:[{a: 1}]}", "{$or:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$or:[{a: 1}, {b: null}]}", "{}");

    assertExpectedRedactSafePortion("{$or:[{a: 1}, {b: null, c:1}]}", "{$or:[{a: 1}, {c:1}]}");

    assertExpectedRedactSafePortion("{$or:[{a: null}, {b: null}]}", "{}");

    assertExpectedRedactSafePortion("{}", "{}");

    // $all and $in
    assertExpectedRedactSafePortion("{a: {$all: [1, 0]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$all: [{$elemMatch: {b:1}}]}}",
                                    "{}");  // could be allowed but currently isn't

    assertExpectedRedactSafePortion("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$in: [1, 0]}}", "{a: {$in: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$in: [1, 0, null]}}", "{}");

    {
        const char* comparisonOps[] = {"$gt", "$lt", "$gte", "$lte", nullptr};
        for (int i = 0; comparisonOps[i]; i++) {
            const char* op = comparisonOps[i];
            assertExpectedRedactSafePortion(string("{a: {") + op + ": 1}}",
                                            string("{a: {") + op + ": 1}}");

            // $elemMatch takes direct expressions ...
            assertExpectedRedactSafePortion(string("{a: {$elemMatch: {") + op + ": 1}}}",
                                            string("{a: {$elemMatch: {") + op + ": 1}}}");

            // ... or top-level style full matches
            assertExpectedRedactSafePortion(string("{a: {$elemMatch: {b: {") + op + ": 1}}}}",
                                            string("{a: {$elemMatch: {b: {") + op + ": 1}}}}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": null}}", "{}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": {}}}", "{}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": []}}", "{}");

            assertExpectedRedactSafePortion(string("{'a.0': {") + op + ": null}}", "{}");

            assertExpectedRedactSafePortion(string("{'a.0.b': {") + op + ": null}}", "{}");
        }
    }
}

TEST_F(DocumentSourceMatchTest, ShouldAddDependenciesOfAllBranchesOfOrClause) {
    auto match =
        DocumentSourceMatch::create(fromjson("{$or: [{a: 1}, {'x.y': {$gt: 4}}]}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("x.y"));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldNotAddPotentialArrayIndexToDependencies) {
    auto match = DocumentSourceMatch::create(
        fromjson("{$or: [{'a.0': 1, '3': 1, 'd.01': 1}, {'b.c.d.1': {$gt: 1}}]}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    // Here we add "a" instead of "a.0". Since we do not support projecting specific array indices,
    // we add the prefix of the path up to the numeric path component instead.
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b.c.d"));
    ASSERT_EQUALS(1U, dependencies.fields.count("3"));
    ASSERT_EQUALS(1U, dependencies.fields.count("d.01"));
    ASSERT_EQUALS(4U, dependencies.fields.size());
}

TEST_F(DocumentSourceMatchTest, TextSearchShouldRequireWholeDocumentAndTextScore) {
    auto match = DocumentSourceMatch::create(fromjson("{$text: {$search: 'hello'} }"), getExpCtx());
    DepsTracker dependencies(DepsTracker::kOnlyTextScore);
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_FIELDS, match->getDependencies(&dependencies));
    ASSERT_EQUALS(true, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldOnlyAddOuterFieldAsDependencyOfImplicitEqualityPredicate) {
    // Parses to {a: {$eq: {notAField: {$gte: 4}}}}.
    auto match = DocumentSourceMatch::create(fromjson("{a: {notAField: {$gte: 4}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldOnlyAddOuterFieldAsDependencyOfClausesWithinElemMatch) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: {$elemMatch: {c: {$gte: 4}}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest,
       ShouldOnlyAddOuterFieldAsDependencyOfClausesWithinInternalSchemaObjectMatch) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "       b: {$_internalSchemaObjectMatch: {"
        "           $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "       }}}"
        "    }}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest,
       ShouldAddWholeDocumentAsDependencyOfClausesWithinInternalSchemaMinProperties) {
    auto query = fromjson("{$_internalSchemaMinProperties: 1}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(true, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest,
       ShouldAddWholeDocumentAsDependencyOfClausesWithinInternalSchemaMaxProperties) {
    auto query = fromjson("{$_internalSchemaMaxProperties: 1}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies1;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies1));
    ASSERT_EQUALS(0U, dependencies1.fields.size());
    ASSERT_EQUALS(true, dependencies1.needWholeDocument);
    ASSERT_EQUALS(false, dependencies1.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    query = fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 1}}}");
    match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies2;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies2));
    ASSERT_EQUALS(1U, dependencies2.fields.size());
    ASSERT_EQUALS(1U, dependencies2.fields.count("a"));
    ASSERT_EQUALS(false, dependencies2.needWholeDocument);
    ASSERT_EQUALS(false, dependencies2.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest,
       ShouldAddWholeDocumentAsDependencyOfClausesWithinInternalSchemaAllowedProperties) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a', 'b'],"
        "namePlaceholder: 'i', patternProperties: [], otherwise: {i: 0}}}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(true, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest,
       ShouldAddWholeDocumentAsDependencyOfClausesWithInternalSchemaRootDocEq) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {a: 1}}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(true, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForClausesWithInternalSchemaType) {
    auto query = fromjson("{a: {$_internalSchemaType: 1}}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForClausesWithInternalSchemaCond) {
    auto query = fromjson("{$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(3U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));
    ASSERT_EQUALS(1U, dependencies.fields.count("c"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForClausesWithInternalSchemaXor) {
    auto query = fromjson("{$_internalSchemaXor: [{a: 1}, {b: 1}, {c: 1}]}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(3U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));
    ASSERT_EQUALS(1U, dependencies.fields.count("c"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForClausesWithEmptyJSONSchema) {
    DepsTracker dependencies;
    auto query = fromjson("{$jsonSchema: {}}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForClausesWithJSONSchemaProperties) {
    DepsTracker dependencies;
    auto query = fromjson("{$jsonSchema: {properties: {a: {type: 'number'}}}}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddCorrectDependenciesForMultiplePredicatesWithJSONSchema) {
    DepsTracker dependencies;
    auto query = fromjson("{$jsonSchema: {properties: {a: {type: 'number'}}}, b: 1}");
    auto match = DocumentSourceMatch::create(query, getExpCtx());
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddOuterFieldToDependenciesIfElemMatchContainsNoFieldNames) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: {$elemMatch: {$gt: 1, $lt: 5}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddNotClausesFieldAsDependency) {
    auto match = DocumentSourceMatch::create(fromjson("{b: {$not: {$gte: 4}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ShouldAddDependenciesOfEachNorClause) {
    auto match = DocumentSourceMatch::create(
        fromjson("{$nor: [{'a.b': {$gte: 4}}, {'b.c': {$in: [1, 2]}}]}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b.c"));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, CommentShouldNotAddAnyDependencies) {
    auto match = DocumentSourceMatch::create(fromjson("{$comment: 'misleading?'}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, ClauseAndedWithCommentShouldAddDependencies) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: 4, $comment: 'irrelevant'}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceMatchTest, MultipleMatchStagesShouldCombineIntoOne) {
    auto match1 = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    auto match2 = DocumentSourceMatch::create(BSON("b" << 1), getExpCtx());
    auto match3 = DocumentSourceMatch::create(BSON("c" << 1), getExpCtx());

    DocumentSourceContainer container;

    // Check initial state
    ASSERT_BSONOBJ_EQ(match1->getQuery(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(match2->getQuery(), BSON("b" << 1));
    ASSERT_BSONOBJ_EQ(match3->getQuery(), BSON("c" << 1));

    container.push_back(match1);
    container.push_back(match2);
    match1->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_BSONOBJ_EQ(match1->getQuery(), fromjson("{'$and': [{a: 1}, {b: 1}]}"));

    container.push_back(match3);
    match1->optimizeAt(container.begin(), &container);
    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_BSONOBJ_EQ(match1->getQuery(), fromjson("{'$and': [{a: 1}, {b: 1}, {c: 1}]}"));
}

TEST_F(DocumentSourceMatchTest, DoesNotPushProjectBeforeSelf) {
    DocumentSourceContainer container;
    auto match = DocumentSourceMatch::create(BSON("_id" << 1), getExpCtx());
    auto project =
        DocumentSourceProject::create(BSON("fullDocument" << true), getExpCtx(), "$project"_sd);

    container.push_back(match);
    container.push_back(project);

    match->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(2U, container.size());
    ASSERT(dynamic_cast<DocumentSourceMatch*>(container.begin()->get()));
    ASSERT(dynamic_cast<DocumentSourceSingleDocumentTransformation*>(
        std::next(container.begin())->get()));
}

TEST_F(DocumentSourceMatchTest, ShouldPropagatePauses) {
    auto match = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    auto mock =
        DocumentSourceMock::createForTest({DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"a", 1}},
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"a", 2}},
                                           Document{{"a", 2}},
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"a", 1}}},
                                          getExpCtx());

    auto matchStage = exec::agg::buildStage(match);
    auto mockStage = exec::agg::buildStage(mock);

    matchStage->setSource(mockStage.get());

    ASSERT_TRUE(matchStage->getNext().isPaused());
    ASSERT_TRUE(matchStage->getNext().isAdvanced());
    ASSERT_TRUE(matchStage->getNext().isPaused());

    // {a: 2} doesn't match, should go directly to the next pause.
    ASSERT_TRUE(matchStage->getNext().isPaused());
    ASSERT_TRUE(matchStage->getNext().isAdvanced());
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldCorrectlyJoinWithSubsequentMatch) {
    const auto match = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    const auto secondMatch = DocumentSourceMatch::create(BSON("b" << 1), getExpCtx());

    match->joinMatchWith(secondMatch, MatchExpression::MatchType::AND);

    const auto mock = DocumentSourceMock::createForTest({Document{{"a", 1}, {"b", 1}},
                                                         Document{{"a", 2}, {"b", 1}},
                                                         Document{{"a", 1}, {"b", 2}},
                                                         Document{{"a", 2}, {"b", 2}}},
                                                        getExpCtx());

    auto matchStage = exec::agg::buildStage(match);
    auto mockStage = exec::agg::buildStage(mock);

    matchStage->setSource(mockStage.get());

    // The first result should match.
    auto next = matchStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", 1}, {"b", 1}}));

    // The rest should not match.
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, RepeatedJoinWithShouldNotNestAnds) {
    auto match1 = DocumentSourceMatch::create(fromjson("{}"), getExpCtx());
    DocumentSourceContainer container{
        match1,
        DocumentSourceMatch::create(fromjson("{}"), getExpCtx()),
        DocumentSourceMatch::create(fromjson("{a: 1}"), getExpCtx()),
        DocumentSourceMatch::create(fromjson("{b: 1}"), getExpCtx()),
        DocumentSourceMatch::create(fromjson("{$and: [{c: 1}, {d: 1}]}"), getExpCtx()),
        DocumentSourceMatch::create(fromjson("{$or: [{e: 1}, {f: 1}]}"), getExpCtx()),
        DocumentSourceMatch::create(fromjson("{}"), getExpCtx()),
        DocumentSourceMatch::create(
            fromjson("{$and: [{g: 1}, {h: 1}], $or: [{i: 1}, {j: 1}], $and: [{k: 1}, {l: 1}]}"),
            getExpCtx()),
        DocumentSourceMatch::create(fromjson("{$and: [{m: 1}, {$and: [{n: 1}, {o: 1}]}]}"),
                                    getExpCtx())};

    // Call optimizeAt() repeatedly to trigger joinWith() behavior
    for (size_t i = 0; i < 8; i++) {
        match1->optimizeAt(container.begin(), &container);
    }

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_BSONOBJ_EQ(
        match1->getQuery(),
        fromjson("{'$and': [{}, {}, {a: 1}, {b: 1}, {c: 1}, {d: 1}, {$or: [{e: 1}, {f: 1}]}, {}, "
                 "{g: 1}, {h: 1}, {$or: [{i: 1}, {j: 1}]}, {k: 1}, {l: 1}, {m: 1}, {$and: [{n: 1}, "
                 "{o: 1}]}]}"));
}

DEATH_TEST_REGEX_F(DocumentSourceMatchTest,
                   ShouldFailToDescendExpressionOnPathThatIsNotACommonPrefix,
                   "Tripwire assertion.*Expected 'a' to be a prefix of 'b.c', but it is not.") {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a.b" << 1 << "b.c" << 1);
    const auto matchExpression =
        unittest::assertGet(MatchExpressionParser::parse(matchSpec, expCtx));
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

DEATH_TEST_REGEX_F(
    DocumentSourceMatchTest,
    ShouldFailToDescendExpressionOnPathThatContainsElemMatchWithObject,
    "Tripwire assertion.*The given match expression has a node that represents a partial path.") {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a" << BSON("$elemMatch" << BSON("a.b" << 1)));
    const auto matchExpression =
        unittest::assertGet(MatchExpressionParser::parse(matchSpec, expCtx));
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

DEATH_TEST_REGEX_F(DocumentSourceMatchTest,
                   ShouldFailToDescendExpressionOnPathThatContainsElemMatchWithValue,
                   "Tripwire assertion.") {
    const auto expCtx = getExpCtx();
    // We will either hit the assertion that $elemMatch is not allowed to be descended on or the
    // assertion that the path of the '$gt' expression (empty path) is not prefixed by 'a'
    const auto matchSpec = BSON("a" << BSON("$elemMatch" << BSON("$gt" << 0)));
    const auto matchExpression =
        unittest::assertGet(MatchExpressionParser::parse(matchSpec, expCtx));
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

TEST_F(DocumentSourceMatchTest, ShouldMatchCorrectlyAfterDescendingMatch) {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a.b" << 1 << "a.c" << 1 << "a.d" << 1);
    const auto matchExpression =
        unittest::assertGet(MatchExpressionParser::parse(matchSpec, expCtx));

    const auto descendedMatch =
        DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
    const auto mock = DocumentSourceMock::createForTest(
        {Document{{"b", 1}, {"c", 1}, {"d", 1}},
         Document{{"b", 1}, {"a", Document{{"c", 1}}}, {"d", 1}},
         Document{{"a", Document{{"b", 1}}}, {"a", Document{{"c", 1}}}, {"d", 1}},
         Document{{"a", Document{{"b", 1}}}, {"a", Document{{"c", 1}}}, {"a", Document{{"d", 1}}}}},
        getExpCtx());

    auto descendedMatchStage = exec::agg::buildStage(descendedMatch);
    auto mockStage = exec::agg::buildStage(mock);

    descendedMatchStage->setSource(mockStage.get());

    auto next = descendedMatchStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"b", 1}, {"c", 1}, {"d", 1}}));

    ASSERT_TRUE(descendedMatchStage->getNext().isEOF());
    ASSERT_TRUE(descendedMatchStage->getNext().isEOF());
    ASSERT_TRUE(descendedMatchStage->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldCorrectlyEvaluateElemMatchPredicate) {
    const auto match =
        DocumentSourceMatch::create(BSON("a" << BSON("$elemMatch" << BSON("b" << 1))), getExpCtx());

    const std::vector<Document> matchingVector = {Document{{"b", 0}}, Document{{"b", 1}}};
    const std::vector<Document> nonMatchingVector = {Document{{"b", 0}}, Document{{"b", 2}}};
    const auto mock = DocumentSourceMock::createForTest(
        {Document{{"a", matchingVector}}, Document{{"a", nonMatchingVector}}, Document{{"a", 1}}},
        getExpCtx());

    auto matchStage = exec::agg::buildStage(match);
    auto mockStage = exec::agg::buildStage(mock);

    matchStage->setSource(mockStage.get());

    // The first result should match.
    auto next = matchStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", matchingVector}}));

    // The rest should not match.
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldCorrectlyEvaluateJSONSchemaPredicate) {
    const auto match = DocumentSourceMatch::create(
        fromjson("{$jsonSchema: {properties: {a: {type: 'number'}}}}"), getExpCtx());

    const auto mock = DocumentSourceMock::createForTest(
        {Document{{"a", 1}}, Document{{"a", "str"_sd}}, Document{{"a", {Document{{{}, 1}}}}}},
        getExpCtx());

    auto matchStage = exec::agg::buildStage(match);
    auto mockStage = exec::agg::buildStage(mock);

    matchStage->setSource(mockStage.get());

    // The first result should match.
    auto next = matchStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", 1}}));

    // The rest should not match.
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
    ASSERT_TRUE(matchStage->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldShowOptimizationsInExplainOutputWhenOptimized) {
    const auto match = DocumentSourceMatch::create(fromjson("{$and: [{a: 1}]}"), getExpCtx());

    auto optimizedMatch = match->optimize();

    auto expectedMatch = fromjson("{$match: {a:{$eq: 1}}}");

    ASSERT_VALUE_EQ(
        Value((static_cast<DocumentSourceMatch*>(optimizedMatch.get()))
                  ->serialize(SerializationOptions{.verbosity = boost::make_optional(
                                                       ExplainOptions::Verbosity::kQueryPlanner)})),
        Value(expectedMatch));
}

TEST_F(DocumentSourceMatchTest, RedactionWithAnd) {
    auto spec = fromjson(R"({
        $match: {
            $and: [
                {
                    "a.c": "abc"
                },
                {
                    "b": {
                        $gt: 10
                    }
                }
            ]
        }})");
    auto docSource = DocumentSourceMatch::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$match": {
                "$and": [
                    {
                        "HASH<a>.HASH<c>": {
                            "$eq": "?string"
                        }
                    },
                    {
                        "HASH<b>": {
                            "$gt": "?number"
                        }
                    }
                ]
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceMatchTest, RedactionWithExprPipeline) {
    auto spec = fromjson(R"({
        $match: {
            $expr: {
                $eq: [
                    '$foo',
                    '$bar'
                ]
            }
        }
    })");
    auto docSource = DocumentSourceMatch::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$match": {
                "$expr": {
                    "$eq": [
                        "$HASH<foo>",
                        "$HASH<bar>"
                    ]
                }
            }
        })",
        redact(*docSource));
}

}  // namespace
}  // namespace mongo
