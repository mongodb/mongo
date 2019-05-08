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

#include <boost/intrusive_ptr.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;

class ReplaceRootBasics : public AggregationContextFixture {
protected:
    intrusive_ptr<DocumentSource> createReplaceRoot(const BSONObj& replaceRoot) {
        BSONObj spec = BSON(DocumentSourceReplaceRoot::kStageName << replaceRoot);
        BSONElement specElement = spec.firstElement();
        return DocumentSourceReplaceRoot::createFromBson(specElement, getExpCtx());
    }

    /**
     * Assert 'source' consistently reports it is exhausted.
     */
    void assertExhausted(const boost::intrusive_ptr<DocumentSource>& source) const {
        ASSERT(source->getNext().isEOF());
        ASSERT(source->getNext().isEOF());
        ASSERT(source->getNext().isEOF());
    }
};

// Verify that sending $newRoot a field path that contains an object in the document results
// in the replacement of the root with that object.
TEST_F(ReplaceRootBasics, FieldPathAsNewRootPromotesSubdocument) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a"));
    Document subdoc = Document{{"b", 1}, {"c", "hello"_sd}, {"d", Document{{"e", 2}}}};
    auto mock = DocumentSourceMock::createForTest(Document{{"a", subdoc}});
    replaceRoot->setSource(mock.get());

    auto next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), subdoc);
    assertExhausted(replaceRoot);
}

// Verify that sending $newRoot a dotted field path that contains an object in the document results
// in the replacement of the root with that object.
TEST_F(ReplaceRootBasics, DottedFieldPathAsNewRootPromotesSubdocument) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a.b"));
    // source document: {a: {b: {c: 3}}}
    Document subdoc = Document{{"c", 3}};
    auto mock = DocumentSourceMock::createForTest(Document{{"a", Document{{"b", subdoc}}}});
    replaceRoot->setSource(mock.get());

    auto next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), subdoc);
    assertExhausted(replaceRoot);
}

// Verify that sending $newRoot a dotted field path that contains an object in two different
// documents results in the replacement of the root with that object in both documents.
TEST_F(ReplaceRootBasics, FieldPathAsNewRootPromotesSubdocumentInMultipleDocuments) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a"));
    Document subdoc1 = Document{{"b", 1}, {"c", 2}};
    Document subdoc2 = Document{{"b", 3}, {"c", 4}};
    auto mock =
        DocumentSourceMock::createForTest({Document{{"a", subdoc1}}, Document{{"a", subdoc2}}});
    replaceRoot->setSource(mock.get());

    // Verify that the first document that comes out is the first document we put in.
    auto next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), subdoc1);

    next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), subdoc2);

    assertExhausted(replaceRoot);
}

// Verify that when newRoot contains an expression object, the document is replaced with that
// object.
TEST_F(ReplaceRootBasics, ExpressionObjectForNewRootReplacesRootWithThatObject) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot" << BSON("b" << 1)));
    auto mock = DocumentSourceMock::createForTest(Document{{"a", 2}});
    replaceRoot->setSource(mock.get());

    auto next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"b", 1}}));
    assertExhausted(replaceRoot);

    BSONObj newObject = BSON("a" << 1 << "b" << 2 << "arr" << BSON_ARRAY(3 << 4 << 5));
    replaceRoot = createReplaceRoot(BSON("newRoot" << newObject));
    mock = DocumentSourceMock::createForTest(Document{{"c", 2}});
    replaceRoot->setSource(mock.get());

    next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), Document(newObject));
    assertExhausted(replaceRoot);

    replaceRoot = createReplaceRoot(BSON("newRoot" << BSON("a" << BSON("b" << 1))));
    mock = DocumentSourceMock::createForTest(Document{{"c", 2}});
    replaceRoot->setSource(mock.get());

    next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", Document{{"b", 1}}}}));
    assertExhausted(replaceRoot);

    replaceRoot = createReplaceRoot(BSON("newRoot" << BSON("a" << 2)));
    mock = DocumentSourceMock::createForTest(Document{{"b", 2}});
    replaceRoot->setSource(mock.get());

    next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", 2}}));
    assertExhausted(replaceRoot);
}

// Verify that when newRoot contains a system variable, the document is replaced with the correct
// object corresponding to that system variable.
TEST_F(ReplaceRootBasics, SystemVariableForNewRootReplacesRootWithThatObject) {
    // System variables
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$$CURRENT"));
    Document inputDoc = Document{{"b", 2}};
    auto mock = DocumentSourceMock::createForTest({inputDoc});
    replaceRoot->setSource(mock.get());

    auto next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), inputDoc);
    assertExhausted(replaceRoot);

    replaceRoot = createReplaceRoot(BSON("newRoot"
                                         << "$$ROOT"));
    mock = DocumentSourceMock::createForTest({inputDoc});
    replaceRoot->setSource(mock.get());

    next = replaceRoot->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), inputDoc);
    assertExhausted(replaceRoot);
}

TEST_F(ReplaceRootBasics, ShouldPropagatePauses) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$$ROOT"));
    auto mock =
        DocumentSourceMock::createForTest({Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document(),
                                           Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           DocumentSource::GetNextResult::makePauseExecution()});
    replaceRoot->setSource(mock.get());

    ASSERT_TRUE(replaceRoot->getNext().isAdvanced());
    ASSERT_TRUE(replaceRoot->getNext().isPaused());
    ASSERT_TRUE(replaceRoot->getNext().isAdvanced());
    ASSERT_TRUE(replaceRoot->getNext().isAdvanced());
    ASSERT_TRUE(replaceRoot->getNext().isPaused());
    ASSERT_TRUE(replaceRoot->getNext().isPaused());

    assertExhausted(replaceRoot);
}

// Verify that when the expression at newRoot does not resolve to an object, as per the spec we
// throw a user assertion.
TEST_F(ReplaceRootBasics, ErrorsWhenNewRootDoesNotEvaluateToAnObject) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a"));

    // A string is not an object.
    auto mock = DocumentSourceMock::createForTest(Document{{"a", "hello"_sd}});
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);

    // An integer is not an object.
    mock = DocumentSourceMock::createForTest(Document{{"a", 5}});
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);

    // Literals are not objects.
    replaceRoot = createReplaceRoot(BSON("newRoot" << BSON("$literal" << 1)));
    mock = DocumentSourceMock::createForTest(Document());
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);
    assertExhausted(replaceRoot);

    // Most operator expressions do not resolve to objects.
    replaceRoot = createReplaceRoot(BSON("newRoot" << BSON("$and"
                                                           << "$a")));
    mock = DocumentSourceMock::createForTest(Document{{"a", true}});
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);
    assertExhausted(replaceRoot);
}

// Verify that when newRoot contains a field path and that field path doesn't exist, we throw a user
// error. This error happens whenever the expression evaluates to a "missing" Value.
TEST_F(ReplaceRootBasics, ErrorsIfNewRootFieldPathDoesNotExist) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a"));

    auto mock = DocumentSourceMock::createForTest(Document());
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);
    assertExhausted(replaceRoot);

    mock = DocumentSourceMock::createForTest(Document{{"e", Document{{"b", Document{{"c", 3}}}}}});
    replaceRoot->setSource(mock.get());
    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);
    assertExhausted(replaceRoot);
}

// Verify that the only dependent field is the root we are replacing with.
TEST_F(ReplaceRootBasics, OnlyDependentFieldIsNewRoot) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a.b"));
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_FIELDS,
                  replaceRoot->getDependencies(&dependencies));

    // Should only depend on field a.b
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
    ASSERT_EQUALS(0U, dependencies.fields.count("a"));
    ASSERT_EQUALS(0U, dependencies.fields.count("b"));

    // Should not need any other fields.
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

TEST_F(ReplaceRootBasics, ReplaceRootModifiesAllFields) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$a"));
    auto modifiedPaths = replaceRoot->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllPaths);
    ASSERT_EQUALS(0U, modifiedPaths.paths.size());
}

TEST_F(ReplaceRootBasics, ReplaceRootWithRemoveSystemVariableThrows) {
    auto replaceRoot = createReplaceRoot(BSON("newRoot"
                                              << "$$REMOVE"));
    Document inputDoc = Document{{"b", 2}};
    auto mock = DocumentSourceMock::createForTest({inputDoc});
    replaceRoot->setSource(mock.get());

    ASSERT_THROWS_CODE(replaceRoot->getNext(), AssertionException, 40228);
}

/**
 * Fixture to test error cases of initializing the $replaceRoot stage.
 */
class ReplaceRootSpec : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createReplaceRoot(const BSONObj& replaceRootSpec) {
        return DocumentSourceReplaceRoot::createFromBson(replaceRootSpec.firstElement(),
                                                         getExpCtx());
    }

    intrusive_ptr<DocumentSource> createReplaceWith(std::string fieldPath) {
        BSONObj spec = BSON(DocumentSourceReplaceRoot::kAliasNameReplaceWith << fieldPath);
        return DocumentSourceReplaceRoot::createFromBson(spec.firstElement(), getExpCtx());
    }

    intrusive_ptr<DocumentSource> createReplaceWith(BSONObj expression) {
        BSONObj spec = BSON(DocumentSourceReplaceRoot::kAliasNameReplaceWith << expression);
        return DocumentSourceReplaceRoot::createFromBson(spec.firstElement(), getExpCtx());
    }


    BSONObj createSpec(BSONObj spec) {
        return BSON("$replaceRoot" << spec);
    }

    BSONObj createFullSpec(BSONObj spec) {
        return BSON("$replaceRoot" << BSON("newRoot" << spec));
    }
};

// Verify that the creation of a $replaceRoot stage requires an object specification
TEST_F(ReplaceRootSpec, CreationRequiresObjectSpecification) {
    ASSERT_THROWS_CODE(createReplaceRoot(BSON("$replaceRoot" << 1)), AssertionException, 40229);
    ASSERT_THROWS_CODE(createReplaceRoot(BSON("$replaceRoot"
                                              << "string")),
                       AssertionException,
                       40229);
}

// Verify that the only valid option for the $replaceRoot object specification is newRoot.
TEST_F(ReplaceRootSpec, OnlyValidOptionInObjectSpecIsNewRoot) {
    ASSERT_THROWS_CODE(createReplaceRoot(createSpec(BSON("newRoot"
                                                         << "$a"
                                                         << "root"
                                                         << 2))),
                       AssertionException,
                       40415);
    ASSERT_THROWS_CODE(createReplaceRoot(createSpec(BSON("newRoot"
                                                         << "$a"
                                                         << "path"
                                                         << 2))),
                       AssertionException,
                       40415);
    ASSERT_THROWS_CODE(createReplaceRoot(createSpec(BSON("path"
                                                         << "$a"))),
                       AssertionException,
                       40415);
}

// Verify that $replaceRoot requires a valid expression as input to the newRoot option.
TEST_F(ReplaceRootSpec, RequiresExpressionForNewRootOption) {
    ASSERT_THROWS_CODE(createReplaceRoot(createSpec(BSONObj())), AssertionException, 40414);
    ASSERT_THROWS(createReplaceRoot(createSpec(BSON("newRoot"
                                                    << "$$$a"))),
                  AssertionException);
    ASSERT_THROWS(createReplaceRoot(createSpec(BSON("newRoot"
                                                    << "$$a"))),
                  AssertionException);
    ASSERT_THROWS(createReplaceRoot(createFullSpec(BSON("$map" << BSON("a" << 1)))),
                  AssertionException);
}

// Verify that newRoot accepts all types of expressions.
TEST_F(ReplaceRootSpec, NewRootAcceptsAllTypesOfExpressions) {
    // Field Path and system variables
    ASSERT_TRUE(createReplaceRoot(createSpec(BSON("newRoot"
                                                  << "$a.b.c.d.e"))));
    ASSERT_TRUE(createReplaceRoot(createSpec(BSON("newRoot"
                                                  << "$$CURRENT"))));

    // Literals
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("$literal" << 1))));

    // Expression Objects
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("a" << BSON("b" << 1)))));

    // Operator Expressions
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("$and"
                                                      << "$a"))));
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("$gt" << BSON_ARRAY("$a" << 1)))));
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("$sqrt"
                                                      << "$a"))));

    // Accumulators
    ASSERT_TRUE(createReplaceRoot(createFullSpec(BSON("$sum"
                                                      << "$a"))));
}

TEST_F(ReplaceRootSpec, ReplaceWithAcceptsAllTypesOfExpressions) {
    // Field Path and system variables
    ASSERT_TRUE(createReplaceWith("$a.b.c.d.e"));
    ASSERT_TRUE(createReplaceWith("$$CURRENT"));

    // Literals
    ASSERT_TRUE(createReplaceWith(BSON("$literal" << 1)));

    // Expression Objects
    ASSERT_TRUE(createReplaceWith(BSON("a" << BSON("b" << 1))));

    // Operator Expressions
    ASSERT_TRUE(createReplaceWith(BSON("$and"
                                       << "$a")));
    ASSERT_TRUE(createReplaceWith(BSON("$gt" << BSON_ARRAY("$a" << 1))));
    ASSERT_TRUE(createReplaceWith(BSON("$sqrt"
                                       << "$a")));

    // Accumulators
    ASSERT_TRUE(createReplaceWith(BSON("$sum"
                                       << "$a")));
}

// Verify that $replaceRoot round trips through serialization.
TEST_F(ReplaceRootSpec, ReplaceRootRoundTrips) {
    auto replaceRoot = createReplaceRoot(createSpec(BSON("newRoot"
                                                         << "$a.b.c.d.e")));
    ASSERT_EQ(replaceRoot->getSourceName(), DocumentSourceReplaceRoot::kStageName);
    std::vector<Value> serialization;
    replaceRoot->serializeToArray(serialization);
    replaceRoot = createReplaceRoot(serialization[0].getDocument().toBson());
    ASSERT_EQ(replaceRoot->getSourceName(), DocumentSourceReplaceRoot::kStageName);
}

// Verify that $replaceWith round trips through serialization.
TEST_F(ReplaceRootSpec, ReplaceWithRoundTrips) {
    auto replaceWith = createReplaceWith("$newRoot");
    // We should always create a stage with the name '$replaceRoot' internally, even if the alias is
    // used.
    ASSERT_EQ(replaceWith->getSourceName(), DocumentSourceReplaceRoot::kStageName);

    std::vector<Value> serialization;
    replaceWith->serializeToArray(serialization);
    replaceWith = createReplaceRoot(serialization[0].getDocument().toBson());
    // While the stage that has round-tripped through serialization should mean the same thing, it
    // will have a different name since it always serializes to the full $replaceRoot syntax.
    ASSERT_EQ(replaceWith->getSourceName(), DocumentSourceReplaceRoot::kStageName);
}

}  // namespace
}  // namespace mongo
