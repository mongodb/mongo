/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::string;
using std::unique_ptr;
using std::vector;

static const char* const ns = "unittests.document_source_group_tests";

/**
 * Fixture for testing execution of the $unwind stage. Note this cannot inherit from
 * AggregationContextFixture, since that inherits from unittest::Test, and this fixture is still
 * being used for old-style tests manually added to the suite below.
 */
class CheckResultsBase {
public:
    CheckResultsBase()
        : _queryServiceContext(stdx::make_unique<QueryTestServiceContext>()),
          _opCtx(_queryServiceContext->makeOperationContext()),
          _ctx(new ExpressionContextForTest(_opCtx.get(),
                                            AggregationRequest(NamespaceString(ns), {}))) {}

    virtual ~CheckResultsBase() {}

    void run() {
        // Once with the simple syntax.
        createSimpleUnwind();
        assertResultsMatch(expectedResultSet(false, false));

        // Once with the full syntax.
        createUnwind(false, false);
        assertResultsMatch(expectedResultSet(false, false));

        // Once with the preserveNullAndEmptyArrays parameter.
        createUnwind(true, false);
        assertResultsMatch(expectedResultSet(true, false));

        // Once with the includeArrayIndex parameter.
        createUnwind(false, true);
        assertResultsMatch(expectedResultSet(false, true));

        // Once with both the preserveNullAndEmptyArrays and includeArrayIndex parameters.
        createUnwind(true, true);
        assertResultsMatch(expectedResultSet(true, true));
    }

protected:
    virtual string unwindFieldPath() const {
        return "$a";
    }

    virtual string indexPath() const {
        return "index";
    }

    virtual deque<DocumentSource::GetNextResult> inputData() {
        return {};
    }

    /**
     * Returns a json string representing the expected results for a normal $unwind without any
     * options.
     */
    virtual string expectedResultSetString() const {
        return "[]";
    }

    /**
     * Returns a json string representing the expected results for a $unwind with the
     * preserveNullAndEmptyArrays parameter set.
     */
    virtual string expectedPreservedResultSetString() const {
        return expectedResultSetString();
    }

    /**
     * Returns a json string representing the expected results for a $unwind with the
     * includeArrayIndex parameter set.
     */
    virtual string expectedIndexedResultSetString() const {
        return "[]";
    }

    /**
     * Returns a json string representing the expected results for a $unwind with both the
     * preserveNullAndEmptyArrays and the includeArrayIndex parameters set.
     */
    virtual string expectedPreservedIndexedResultSetString() const {
        return expectedIndexedResultSetString();
    }

    intrusive_ptr<ExpressionContextForTest> ctx() const {
        return _ctx;
    }

private:
    /**
     * Initializes '_unwind' using the simple '{$unwind: '$path'}' syntax.
     */
    void createSimpleUnwind() {
        auto specObj = BSON("$unwind" << unwindFieldPath());
        _unwind = static_cast<DocumentSourceUnwind*>(
            DocumentSourceUnwind::createFromBson(specObj.firstElement(), ctx()).get());
        checkBsonRepresentation(false, false);
    }

    /**
     * Initializes '_unwind' using the full '{$unwind: {path: '$path'}}' syntax.
     */
    void createUnwind(bool preserveNullAndEmptyArrays, bool includeArrayIndex) {
        auto specObj =
            DOC("$unwind" << DOC("path" << unwindFieldPath() << "preserveNullAndEmptyArrays"
                                        << preserveNullAndEmptyArrays
                                        << "includeArrayIndex"
                                        << (includeArrayIndex ? Value(indexPath()) : Value())));
        _unwind = static_cast<DocumentSourceUnwind*>(
            DocumentSourceUnwind::createFromBson(specObj.toBson().firstElement(), ctx()).get());
        checkBsonRepresentation(preserveNullAndEmptyArrays, includeArrayIndex);
    }

    /**
     * Extracts the documents from the $unwind stage, and asserts the actual results match the
     * expected results.
     *
     * '_unwind' must be initialized before calling this method.
     */
    void assertResultsMatch(BSONObj expectedResults) {
        auto source = DocumentSourceMock::create(inputData());
        _unwind->setSource(source.get());
        // Load the results from the DocumentSourceUnwind.
        vector<Document> resultSet;
        for (auto output = _unwind->getNext(); output.isAdvanced(); output = _unwind->getNext()) {
            // Get the current result.
            resultSet.push_back(output.releaseDocument());
        }
        // Verify the DocumentSourceUnwind is exhausted.
        assertEOF();

        // Convert results to BSON once they all have been retrieved (to detect any errors resulting
        // from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (vector<Document>::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << *i;
        }
        // Check the result set.
        ASSERT_BSONOBJ_EQ(expectedResults, bsonResultSet.arr());
    }

    /**
     * Check that the BSON representation generated by the source matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(bool preserveNullAndEmptyArrays, bool includeArrayIndex) {
        vector<Value> arr;
        _unwind->serializeToArray(arr);
        BSONObj generatedSpec = Value(arr[0]).getDocument().toBson();
        ASSERT_BSONOBJ_EQ(expectedSerialization(preserveNullAndEmptyArrays, includeArrayIndex),
                          generatedSpec);
    }

    BSONObj expectedSerialization(bool preserveNullAndEmptyArrays, bool includeArrayIndex) const {
        return DOC("$unwind" << DOC("path" << Value(unwindFieldPath())
                                           << "preserveNullAndEmptyArrays"
                                           << (preserveNullAndEmptyArrays ? Value(true) : Value())
                                           << "includeArrayIndex"
                                           << (includeArrayIndex ? Value(indexPath()) : Value())))
            .toBson();
    }

    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertEOF() const {
        ASSERT(_unwind->getNext().isEOF());
        ASSERT(_unwind->getNext().isEOF());
        ASSERT(_unwind->getNext().isEOF());
    }

    BSONObj expectedResultSet(bool preserveNullAndEmptyArrays, bool includeArrayIndex) const {
        string expectedResultsString;
        if (preserveNullAndEmptyArrays) {
            if (includeArrayIndex) {
                expectedResultsString = expectedPreservedIndexedResultSetString();
            } else {
                expectedResultsString = expectedPreservedResultSetString();
            }
        } else {
            if (includeArrayIndex) {
                expectedResultsString = expectedIndexedResultSetString();
            } else {
                expectedResultsString = expectedResultSetString();
            }
        }
        // fromjson() cannot parse an array, so place the array within an object.
        BSONObj wrappedResult = fromjson(string("{'':") + expectedResultsString + "}");
        return wrappedResult[""].embeddedObject().getOwned();
    }

    unique_ptr<QueryTestServiceContext> _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    intrusive_ptr<ExpressionContextForTest> _ctx;
    intrusive_ptr<DocumentSourceUnwind> _unwind;
};

/** An empty collection produces no results. */
class Empty : public CheckResultsBase {};

/**
 * An empty array does not produce any results normally, but if preserveNullAndEmptyArrays is
 * passed, the document is preserved.
 */
class EmptyArray : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << BSONArray())};
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, index: null}]";
    }
};

/**
 * A missing value does not produce any results normally, but if preserveNullAndEmptyArrays is
 * passed, the document is preserved.
 */
class MissingValue : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0)};
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, index: null}]";
    }
};

/**
 * A null value does not produce any results normally, but if preserveNullAndEmptyArrays is passed,
 * the document is preserved.
 */
class Null : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << BSONNULL)};
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: null}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: null, index: null}]";
    }
};

/**
 * An undefined value does not produce any results normally, but if preserveNullAndEmptyArrays is
 * passed, the document is preserved.
 */
class Undefined : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << BSONUndefined)};
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: undefined}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: undefined, index: null}]";
    }
};

/** Unwind an array with one value. */
class OneValue : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 1}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0}]";
    }
};

/** Unwind an array with two values. */
class TwoValues : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 1}, {_id: 0, a: 2}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0}, {_id: 0, a: 2, index: 1}]";
    }
};

/** Unwind an array with two values, one of which is null. */
class ArrayWithNull : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << BSONNULL))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 1}, {_id: 0, a: null}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0}, {_id: 0, a: null, index: 1}]";
    }
};

/** Unwind two documents with arrays. */
class TwoDocuments : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2)),
                DOC("_id" << 1 << "a" << DOC_ARRAY(3 << 4))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 1}, {_id: 0, a: 2}, {_id: 1, a: 3}, {_id: 1, a: 4}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0}, {_id: 0, a: 2, index: 1},"
               " {_id: 1, a: 3, index: 0}, {_id: 1, a: 4, index: 1}]";
    }
};

/** Unwind an array in a nested document. */
class NestedArray : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC("b" << DOC_ARRAY(1 << 2) << "c" << 3))};
    }
    string unwindFieldPath() const override {
        return "$a.b";
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: {b: 1, c: 3}}, {_id: 0, a: {b: 2, c: 3}}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: {b: 1, c: 3}, index: 0},"
               " {_id: 0, a: {b: 2, c: 3}, index: 1}]";
    }
};

/**
 * A nested path produces no results when there is no sub-document that matches the path, unless
 * preserveNullAndEmptyArrays is specified.
 */
class NonObjectParent : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << 4)};
    }
    string unwindFieldPath() const override {
        return "$a.b";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 4}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 4, index: null}]";
    }
};

/** Unwind an array in a doubly nested document. */
class DoubleNestedArray : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a"
                          << DOC("b" << DOC("d" << DOC_ARRAY(1 << 2) << "e" << 4) << "c" << 3))};
    }
    string unwindFieldPath() const override {
        return "$a.b.d";
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: {b: {d: 1, e: 4}, c: 3}}, {_id: 0, a: {b: {d: 2, e: 4}, c: 3}}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: {b: {d: 1, e: 4}, c: 3}, index: 0}, "
               " {_id: 0, a: {b: {d: 2, e: 4}, c: 3}, index: 1}]";
    }
};

/** Unwind several documents in a row. */
class SeveralDocuments : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2 << 3)),
                DOC("_id" << 1),
                DOC("_id" << 2),
                DOC("_id" << 3 << "a" << DOC_ARRAY(10 << 20)),
                DOC("_id" << 4 << "a" << DOC_ARRAY(30))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 1}, {_id: 0, a: 2}, {_id: 0, a: 3},"
               " {_id: 3, a: 10}, {_id: 3, a: 20},"
               " {_id: 4, a: 30}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 1}, {_id: 0, a: 2}, {_id: 0, a: 3},"
               " {_id: 1},"
               " {_id: 2},"
               " {_id: 3, a: 10}, {_id: 3, a: 20},"
               " {_id: 4, a: 30}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0},"
               " {_id: 0, a: 2, index: 1},"
               " {_id: 0, a: 3, index: 2},"
               " {_id: 3, a: 10, index: 0},"
               " {_id: 3, a: 20, index: 1},"
               " {_id: 4, a: 30, index: 0}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 1, index: 0},"
               " {_id: 0, a: 2, index: 1},"
               " {_id: 0, a: 3, index: 2},"
               " {_id: 1, index: null},"
               " {_id: 2, index: null},"
               " {_id: 3, a: 10, index: 0},"
               " {_id: 3, a: 20, index: 1},"
               " {_id: 4, a: 30, index: 0}]";
    }
};

/** Unwind several more documents in a row. */
class SeveralMoreDocuments : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << BSONNULL),
                DOC("_id" << 1),
                DOC("_id" << 2 << "a" << DOC_ARRAY("a"_sd
                                                   << "b"_sd)),
                DOC("_id" << 3),
                DOC("_id" << 4 << "a" << DOC_ARRAY(1 << 2 << 3)),
                DOC("_id" << 5 << "a" << DOC_ARRAY(4 << 5 << 6)),
                DOC("_id" << 6 << "a" << DOC_ARRAY(7 << 8 << 9)),
                DOC("_id" << 7 << "a" << BSONArray())};
    }
    string expectedResultSetString() const override {
        return "[{_id: 2, a: 'a'}, {_id: 2, a: 'b'},"
               " {_id: 4, a: 1}, {_id: 4, a: 2}, {_id: 4, a: 3},"
               " {_id: 5, a: 4}, {_id: 5, a: 5}, {_id: 5, a: 6},"
               " {_id: 6, a: 7}, {_id: 6, a: 8}, {_id: 6, a: 9}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: null},"
               " {_id: 1},"
               " {_id: 2, a: 'a'}, {_id: 2, a: 'b'},"
               " {_id: 3},"
               " {_id: 4, a: 1}, {_id: 4, a: 2}, {_id: 4, a: 3},"
               " {_id: 5, a: 4}, {_id: 5, a: 5}, {_id: 5, a: 6},"
               " {_id: 6, a: 7}, {_id: 6, a: 8}, {_id: 6, a: 9},"
               " {_id: 7}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 2, a: 'a', index: 0},"
               " {_id: 2, a: 'b', index: 1},"
               " {_id: 4, a: 1, index: 0},"
               " {_id: 4, a: 2, index: 1},"
               " {_id: 4, a: 3, index: 2},"
               " {_id: 5, a: 4, index: 0},"
               " {_id: 5, a: 5, index: 1},"
               " {_id: 5, a: 6, index: 2},"
               " {_id: 6, a: 7, index: 0},"
               " {_id: 6, a: 8, index: 1},"
               " {_id: 6, a: 9, index: 2}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: null, index: null},"
               " {_id: 1, index: null},"
               " {_id: 2, a: 'a', index: 0},"
               " {_id: 2, a: 'b', index: 1},"
               " {_id: 3, index: null},"
               " {_id: 4, a: 1, index: 0},"
               " {_id: 4, a: 2, index: 1},"
               " {_id: 4, a: 3, index: 2},"
               " {_id: 5, a: 4, index: 0},"
               " {_id: 5, a: 5, index: 1},"
               " {_id: 5, a: 6, index: 2},"
               " {_id: 6, a: 7, index: 0},"
               " {_id: 6, a: 8, index: 1},"
               " {_id: 6, a: 9, index: 2},"
               " {_id: 7, index: null}]";
    }
};

/**
 * Test the 'includeArrayIndex' option, where the specified path is part of a sub-object.
 */
class IncludeArrayIndexSubObject : public CheckResultsBase {
    string indexPath() const override {
        return "b.index";
    }
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(0) << "b" << DOC("x" << 100)),
                DOC("_id" << 1 << "a" << 1 << "b" << DOC("x" << 100)),
                DOC("_id" << 2 << "b" << DOC("x" << 100))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {x: 100}}, {_id: 1, a: 1, b: {x: 100}}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {x: 100}}, {_id: 1, a: 1, b: {x: 100}}, {_id: 2, b: {x: 100}}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {x: 100, index: 0}}, {_id: 1, a: 1, b: {x: 100, index: null}}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {x: 100, index: 0}},"
               " {_id: 1, a: 1, b: {x: 100, index: null}},"
               " {_id: 2, b: {x: 100, index: null}}]";
    }
};

/**
 * Test the 'includeArrayIndex' option, where the specified path overrides an existing field.
 */
class IncludeArrayIndexOverrideExisting : public CheckResultsBase {
    string indexPath() const override {
        return "b";
    }
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(0) << "b" << 100),
                DOC("_id" << 1 << "a" << 1 << "b" << 100),
                DOC("_id" << 2 << "b" << 100)};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 100}, {_id: 1, a: 1, b: 100}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 100}, {_id: 1, a: 1, b: 100}, {_id: 2, b: 100}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 0}, {_id: 1, a: 1, b: null}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 0}, {_id: 1, a: 1, b: null}, {_id: 2, b: null}]";
    }
};

/**
 * Test the 'includeArrayIndex' option, where the specified path overrides an existing nested field.
 */
class IncludeArrayIndexOverrideExistingNested : public CheckResultsBase {
    string indexPath() const override {
        return "b.index";
    }
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(0) << "b" << 100),
                DOC("_id" << 1 << "a" << 1 << "b" << 100),
                DOC("_id" << 2 << "b" << 100)};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 100}, {_id: 1, a: 1, b: 100}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 0, b: 100}, {_id: 1, a: 1, b: 100}, {_id: 2, b: 100}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {index: 0}}, {_id: 1, a: 1, b: {index: null}}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0, b: {index: 0}},"
               " {_id: 1, a: 1, b: {index: null}},"
               " {_id: 2, b: {index: null}}]";
    }
};

/**
 * Test the 'includeArrayIndex' option, where the specified path overrides the field that was being
 * unwound.
 */
class IncludeArrayIndexOverrideUnwindPath : public CheckResultsBase {
    string indexPath() const override {
        return "a";
    }
    deque<DocumentSource::GetNextResult> inputData() override {
        return {
            DOC("_id" << 0 << "a" << DOC_ARRAY(5)), DOC("_id" << 1 << "a" << 1), DOC("_id" << 2)};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 5}, {_id: 1, a: 1}]";
    }
    string expectedPreservedResultSetString() const override {
        return "[{_id: 0, a: 5}, {_id: 1, a: 1}, {_id: 2}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0}, {_id: 1, a: null}]";
    }
    string expectedPreservedIndexedResultSetString() const override {
        return "[{_id: 0, a: 0}, {_id: 1, a: null}, {_id: 2, a: null}]";
    }
};

/**
 * Test the 'includeArrayIndex' option, where the specified path is a subfield of the field that was
 * being unwound.
 */
class IncludeArrayIndexWithinUnwindPath : public CheckResultsBase {
    string indexPath() const override {
        return "a.index";
    }
    deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a"
                          << DOC_ARRAY(100 << DOC("b" << 1) << DOC("b" << 1 << "index" << -1)))};
    }
    string expectedResultSetString() const override {
        return "[{_id: 0, a: 100}, {_id: 0, a: {b: 1}}, {_id: 0, a: {b: 1, index: -1}}]";
    }
    string expectedIndexedResultSetString() const override {
        return "[{_id: 0, a: {index: 0}},"
               " {_id: 0, a: {b: 1, index: 1}},"
               " {_id: 0, a: {b: 1, index: 2}}]";
    }
};

/**
 * New-style fixture for testing the $unwind stage. Provides access to an ExpressionContext which
 * can be used to construct DocumentSourceUnwind.
 */
class UnwindStageTest : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createUnwind(BSONObj spec) {
        auto specElem = spec.firstElement();
        return DocumentSourceUnwind::createFromBson(specElem, getExpCtx());
    }
};

TEST_F(UnwindStageTest, AddsUnwoundPathToDependencies) {
    auto unwind =
        DocumentSourceUnwind::create(getExpCtx(), "x.y.z", false, boost::optional<string>("index"));
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, unwind->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(1U, dependencies.fields.count("x.y.z"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(UnwindStageTest, TruncatesOutputSortAtUnwoundPath) {
    auto unwind = DocumentSourceUnwind::create(getExpCtx(), "x.y", false, boost::none);
    auto source = DocumentSourceMock::create();
    source->sorts = {BSON("a" << 1 << "x.y" << 1 << "b" << 1)};

    unwind->setSource(source.get());

    BSONObjSet outputSort = unwind->getOutputSorts();
    ASSERT_EQUALS(1U, outputSort.size());
    ASSERT_EQUALS(1U, outputSort.count(BSON("a" << 1)));
}

TEST_F(UnwindStageTest, ShouldPropagatePauses) {
    const bool includeNullIfEmptyOrMissing = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    auto unwind = DocumentSourceUnwind::create(
        getExpCtx(), "array", includeNullIfEmptyOrMissing, includeArrayIndex);
    auto source =
        DocumentSourceMock::create({Document{{"array", vector<Value>{Value(1), Value(2)}}},
                                    DocumentSource::GetNextResult::makePauseExecution(),
                                    Document{{"array", vector<Value>{Value(1), Value(2)}}}});

    unwind->setSource(source.get());

    ASSERT_TRUE(unwind->getNext().isAdvanced());
    ASSERT_TRUE(unwind->getNext().isAdvanced());

    ASSERT_TRUE(unwind->getNext().isPaused());

    ASSERT_TRUE(unwind->getNext().isAdvanced());
    ASSERT_TRUE(unwind->getNext().isAdvanced());

    ASSERT_TRUE(unwind->getNext().isEOF());
    ASSERT_TRUE(unwind->getNext().isEOF());
}

TEST_F(UnwindStageTest, UnwindOnlyModifiesUnwoundPathWhenNotIncludingIndex) {
    const bool includeNullIfEmptyOrMissing = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    auto unwind = DocumentSourceUnwind::create(
        getExpCtx(), "array", includeNullIfEmptyOrMissing, includeArrayIndex);

    auto modifiedPaths = unwind->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(1U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("array"));
}

TEST_F(UnwindStageTest, UnwindIncludesIndexPathWhenIncludingIndex) {
    const bool includeNullIfEmptyOrMissing = false;
    const boost::optional<std::string> includeArrayIndex = std::string("arrIndex");
    auto unwind = DocumentSourceUnwind::create(
        getExpCtx(), "array", includeNullIfEmptyOrMissing, includeArrayIndex);

    auto modifiedPaths = unwind->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(2U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("array"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("arrIndex"));
}

//
// Error cases.
//

TEST_F(UnwindStageTest, ShouldRejectNonObjectNonString) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << 1)), UserException, 15981);
}

TEST_F(UnwindStageTest, ShouldRejectSpecWithoutPath) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSONObj())), UserException, 28812);
}

TEST_F(UnwindStageTest, ShouldRejectNonStringPath) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path" << 2))), UserException, 28808);
}

TEST_F(UnwindStageTest, ShouldRejectNonDollarPrefixedPath) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind"
                                         << "somePath")),
                       UserException,
                       28818);
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "somePath"))),
                       UserException,
                       28818);
}

TEST_F(UnwindStageTest, ShouldRejectNonBoolPreserveNullAndEmptyArrays) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "preserveNullAndEmptyArrays"
                                                           << 2))),
                       UserException,
                       28809);
}

TEST_F(UnwindStageTest, ShouldRejectNonStringIncludeArrayIndex) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << 2))),
                       UserException,
                       28810);
}

TEST_F(UnwindStageTest, ShouldRejectEmptyStringIncludeArrayIndex) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << ""))),
                       UserException,
                       28810);
}

TEST_F(UnwindStageTest, ShoudlRejectDollarPrefixedIncludeArrayIndex) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << "$"))),
                       UserException,
                       28822);
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << "$path"))),
                       UserException,
                       28822);
}

TEST_F(UnwindStageTest, ShouldRejectUnrecognizedOption) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "preserveNullAndEmptyArrays"
                                                           << true
                                                           << "foo"
                                                           << 3))),
                       UserException,
                       28811);
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "foo"
                                                           << 3))),
                       UserException,
                       28811);
}

class All : public Suite {
public:
    All() : Suite("DocumentSourceUnwindTests") {}
    void setupTests() {
        add<Empty>();
        add<EmptyArray>();
        add<MissingValue>();
        add<Null>();
        add<Undefined>();
        add<OneValue>();
        add<TwoValues>();
        add<ArrayWithNull>();
        add<TwoDocuments>();
        add<NestedArray>();
        add<NonObjectParent>();
        add<DoubleNestedArray>();
        add<SeveralDocuments>();
        add<SeveralMoreDocuments>();
        add<IncludeArrayIndexSubObject>();
        add<IncludeArrayIndexOverrideExisting>();
        add<IncludeArrayIndexOverrideExistingNested>();
        add<IncludeArrayIndexOverrideUnwindPath>();
        add<IncludeArrayIndexWithinUnwindPath>();
    }
};

SuiteInstance<All> myall;

}  // namespace
}  // namespace mongo
