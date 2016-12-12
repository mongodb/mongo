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
#include <map>
#include <string>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
using boost::intrusive_ptr;
using std::deque;
using std::map;
using std::string;
using std::vector;

static const char* const ns = "unittests.document_source_group_tests";

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGroupTest = AggregationContextFixture;

TEST_F(DocumentSourceGroupTest, ShouldBeAbleToPauseLoading) {
    auto expCtx = getExpCtx();
    expCtx->inRouter = true;  // Disallow external sort.
                              // This is the only way to do this in a debug build.
    AccumulationStatement countStatement{"count",
                                         AccumulationStatement::getFactory("$sum"),
                                         ExpressionConstant::create(expCtx, Value(1))};
    auto group = DocumentSourceGroup::create(
        expCtx, ExpressionConstant::create(expCtx, Value(BSONNULL)), {countStatement}, 0);
    auto mock = DocumentSourceMock::create({DocumentSource::GetNextResult::makePauseExecution(),
                                            Document(),
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document(),
                                            Document(),
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document()});
    group->setSource(mock.get());

    // There were 3 pauses, so we should expect 3 paused results before any results can be returned.
    ASSERT_TRUE(group->getNext().isPaused());
    ASSERT_TRUE(group->getNext().isPaused());
    ASSERT_TRUE(group->getNext().isPaused());

    // There were 4 documents, so we expect a count of 4.
    auto result = group->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.releaseDocument(), (Document{{"_id", BSONNULL}, {"count", 4}}));
}

TEST_F(DocumentSourceGroupTest, ShouldBeAbleToPauseLoadingWhileSpilled) {
    auto expCtx = getExpCtx();

    // Allow the $group stage to spill to disk.
    TempDir tempDir("DocumentSourceGroupTest");
    expCtx->tempDir = tempDir.path();
    expCtx->extSortAllowed = true;
    const size_t maxMemoryUsageBytes = 1000;

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    AccumulationStatement pushStatement{"spaceHog",
                                        AccumulationStatement::getFactory("$push"),
                                        ExpressionFieldPath::parse("$largeStr", vps)};
    auto groupByExpression = ExpressionFieldPath::parse("$_id", vps);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, idGen.getIdCount(), maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = DocumentSourceMock::create({Document{{"_id", 0}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"_id", 1}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"_id", 2}, {"largeStr", largeStr}}});
    group->setSource(mock.get());

    // There were 2 pauses, so we should expect 2 paused results before any results can be returned.
    ASSERT_TRUE(group->getNext().isPaused());
    ASSERT_TRUE(group->getNext().isPaused());

    // Now we expect to get the results back, although in no particular order.
    stdx::unordered_set<int> idSet;
    for (auto result = group->getNext(); result.isAdvanced(); result = group->getNext()) {
        idSet.insert(result.releaseDocument()["_id"].coerceToInt());
    }
    ASSERT_TRUE(group->getNext().isEOF());

    ASSERT_EQ(idSet.size(), 3UL);
    ASSERT_EQ(idSet.count(0), 1UL);
    ASSERT_EQ(idSet.count(1), 1UL);
    ASSERT_EQ(idSet.count(2), 1UL);
}

TEST_F(DocumentSourceGroupTest, ShouldErrorIfNotAllowedToSpillToDiskAndResultSetIsTooLarge) {
    auto expCtx = getExpCtx();
    const size_t maxMemoryUsageBytes = 1000;
    expCtx->inRouter = true;  // Disallow external sort.
                              // This is the only way to do this in a debug build.

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    AccumulationStatement pushStatement{"spaceHog",
                                        AccumulationStatement::getFactory("$push"),
                                        ExpressionFieldPath::parse("$largeStr", vps)};
    auto groupByExpression = ExpressionFieldPath::parse("$_id", vps);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, idGen.getIdCount(), maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = DocumentSourceMock::create({Document{{"_id", 0}, {"largeStr", largeStr}},
                                            Document{{"_id", 1}, {"largeStr", largeStr}}});
    group->setSource(mock.get());

    ASSERT_THROWS_CODE(group->getNext(), UserException, 16945);
}

TEST_F(DocumentSourceGroupTest, ShouldCorrectlyTrackMemoryUsageBetweenPauses) {
    auto expCtx = getExpCtx();
    const size_t maxMemoryUsageBytes = 1000;
    expCtx->inRouter = true;  // Disallow external sort.
                              // This is the only way to do this in a debug build.

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    AccumulationStatement pushStatement{"spaceHog",
                                        AccumulationStatement::getFactory("$push"),
                                        ExpressionFieldPath::parse("$largeStr", vps)};
    auto groupByExpression = ExpressionFieldPath::parse("$_id", vps);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, idGen.getIdCount(), maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes / 2, 'x');
    auto mock = DocumentSourceMock::create({Document{{"_id", 0}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"_id", 1}, {"largeStr", largeStr}},
                                            Document{{"_id", 2}, {"largeStr", largeStr}}});
    group->setSource(mock.get());

    // The first getNext() should pause.
    ASSERT_TRUE(group->getNext().isPaused());

    // The next should realize it's used too much memory.
    ASSERT_THROWS_CODE(group->getNext(), UserException, 16945);
}

BSONObj toBson(const intrusive_ptr<DocumentSource>& source) {
    vector<Value> arr;
    source->serializeToArray(arr);
    ASSERT_EQUALS(arr.size(), 1UL);
    return arr[0].getDocument().toBson();
}

class Base {
public:
    Base()
        : _queryServiceContext(stdx::make_unique<QueryTestServiceContext>()),
          _opCtx(_queryServiceContext->makeOperationContext()),
          _ctx(new ExpressionContext(_opCtx.get(), AggregationRequest(NamespaceString(ns), {}))),
          _tempDir("DocumentSourceGroupTest") {}

protected:
    void createGroup(const BSONObj& spec, bool inShard = false, bool inRouter = false) {
        BSONObj namedSpec = BSON("$group" << spec);
        BSONElement specElement = namedSpec.firstElement();

        intrusive_ptr<ExpressionContext> expressionContext =
            new ExpressionContext(_opCtx.get(), AggregationRequest(NamespaceString(ns), {}));
        expressionContext->inShard = inShard;
        expressionContext->inRouter = inRouter;
        // Won't spill to disk properly if it needs to.
        expressionContext->tempDir = _tempDir.path();

        _group = DocumentSourceGroup::createFromBson(specElement, expressionContext);
        _group->injectExpressionContext(expressionContext);
        assertRoundTrips(_group);
    }
    DocumentSourceGroup* group() {
        return static_cast<DocumentSourceGroup*>(_group.get());
    }
    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertEOF(const intrusive_ptr<DocumentSource>& source) const {
        // It should be safe to check doneness multiple times
        ASSERT(source->getNext().isEOF());
        ASSERT(source->getNext().isEOF());
        ASSERT(source->getNext().isEOF());
    }

    intrusive_ptr<ExpressionContext> ctx() const {
        return _ctx;
    }

private:
    /** Check that the group's spec round trips. */
    void assertRoundTrips(const intrusive_ptr<DocumentSource>& group) {
        // We don't check against the spec that generated 'group' originally, because
        // $const operators may be introduced in the first serialization.
        BSONObj spec = toBson(group);
        BSONElement specElement = spec.firstElement();
        intrusive_ptr<DocumentSource> generated =
            DocumentSourceGroup::createFromBson(specElement, ctx());
        ASSERT_BSONOBJ_EQ(spec, toBson(generated));
    }
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    intrusive_ptr<ExpressionContext> _ctx;
    intrusive_ptr<DocumentSource> _group;
    TempDir _tempDir;
};

class ParseErrorBase : public Base {
public:
    virtual ~ParseErrorBase() {}
    void run() {
        ASSERT_THROWS(createGroup(spec()), UserException);
    }

protected:
    virtual BSONObj spec() = 0;
};

class ExpressionBase : public Base {
public:
    virtual ~ExpressionBase() {}
    void run() {
        createGroup(spec());
        auto source = DocumentSourceMock::create(Document(doc()));
        group()->setSource(source.get());
        // A group result is available.
        auto next = group()->getNext();
        ASSERT(next.isAdvanced());
        // The constant _id value from the $group spec is passed through.
        ASSERT_BSONOBJ_EQ(expected(), next.getDocument().toBson());
    }

protected:
    virtual BSONObj doc() = 0;
    virtual BSONObj spec() = 0;
    virtual BSONObj expected() = 0;
};

class IdConstantBase : public ExpressionBase {
    virtual BSONObj doc() {
        return BSONObj();
    }
    virtual BSONObj expected() {
        // Since spec() specifies a constant _id, its value will be passed through.
        return spec();
    }
};

/** $group spec is not an object. */
class NonObject : public Base {
public:
    void run() {
        BSONObj spec = BSON("$group"
                            << "foo");
        BSONElement specElement = spec.firstElement();
        ASSERT_THROWS(DocumentSourceGroup::createFromBson(specElement, ctx()), UserException);
    }
};

/** $group spec is an empty object. */
class EmptySpec : public ParseErrorBase {
    BSONObj spec() {
        return BSONObj();
    }
};

/** $group _id is an empty object. */
class IdEmptyObject : public IdConstantBase {
    BSONObj spec() {
        return BSON("_id" << BSONObj());
    }
};

/** $group _id is computed from an object expression. */
class IdObjectExpression : public ExpressionBase {
    BSONObj doc() {
        return BSON("a" << 6);
    }
    BSONObj spec() {
        return BSON("_id" << BSON("z"
                                  << "$a"));
    }
    BSONObj expected() {
        return BSON("_id" << BSON("z" << 6));
    }
};

/** $group _id is specified as an invalid object expression. */
class IdInvalidObjectExpression : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << BSON("$add" << 1 << "$and" << 1));
    }
};

/** $group with two _id specs. */
class TwoIdSpecs : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "_id" << 2);
    }
};

/** $group _id is the empty string. */
class IdEmptyString : public IdConstantBase {
    BSONObj spec() {
        return BSON("_id"
                    << "");
    }
};

/** $group _id is a string constant. */
class IdStringConstant : public IdConstantBase {
    BSONObj spec() {
        return BSON("_id"
                    << "abc");
    }
};

/** $group _id is a field path expression. */
class IdFieldPath : public ExpressionBase {
    BSONObj doc() {
        return BSON("a" << 5);
    }
    BSONObj spec() {
        return BSON("_id"
                    << "$a");
    }
    BSONObj expected() {
        return BSON("_id" << 5);
    }
};

/** $group with _id set to an invalid field path. */
class IdInvalidFieldPath : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id"
                    << "$a..");
    }
};

/** $group _id is a numeric constant. */
class IdNumericConstant : public IdConstantBase {
    BSONObj spec() {
        return BSON("_id" << 2);
    }
};

/** $group _id is an array constant. */
class IdArrayConstant : public IdConstantBase {
    BSONObj spec() {
        return BSON("_id" << BSON_ARRAY(1 << 2));
    }
};

/** $group _id is a regular expression (not supported). */
class IdRegularExpression : public IdConstantBase {
    BSONObj spec() {
        return fromjson("{_id:/a/}");
    }
};

/** The name of an aggregate field is specified with a $ prefix. */
class DollarAggregateFieldName : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "$foo" << BSON("$sum" << 1));
    }
};

/** An aggregate field spec that is not an object. */
class NonObjectAggregateSpec : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "a" << 1);
    }
};

/** An aggregate field spec that is not an object. */
class EmptyObjectAggregateSpec : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "a" << BSONObj());
    }
};

/** An aggregate field spec with an invalid accumulator operator. */
class BadAccumulator : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "a" << BSON("$bad" << 1));
    }
};

/** An aggregate field spec with an array argument. */
class SumArray : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "a" << BSON("$sum" << BSONArray()));
    }
};

/** Multiple accumulator operators for a field. */
class MultipleAccumulatorsForAField : public ParseErrorBase {
    BSONObj spec() {
        return BSON("_id" << 1 << "a" << BSON("$sum" << 1 << "$push" << 1));
    }
};

/** Aggregation using duplicate field names is allowed currently. */
class DuplicateAggregateFieldNames : public ExpressionBase {
    BSONObj doc() {
        return BSONObj();
    }
    BSONObj spec() {
        return BSON("_id" << 0 << "z" << BSON("$sum" << 1) << "z" << BSON("$push" << 1));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "z" << 1 << "z" << BSON_ARRAY(1));
    }
};

/** Aggregate the value of an object expression. */
class AggregateObjectExpression : public ExpressionBase {
    BSONObj doc() {
        return BSON("a" << 6);
    }
    BSONObj spec() {
        return BSON("_id" << 0 << "z" << BSON("$first" << BSON("x"
                                                               << "$a")));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "z" << BSON("x" << 6));
    }
};

/** Aggregate the value of an operator expression. */
class AggregateOperatorExpression : public ExpressionBase {
    BSONObj doc() {
        return BSON("a" << 6);
    }
    BSONObj spec() {
        return BSON("_id" << 0 << "z" << BSON("$first"
                                              << "$a"));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "z" << 6);
    }
};

struct ValueCmp {
    bool operator()(const Value& a, const Value& b) const {
        return ValueComparator().evaluate(a < b);
    }
};
typedef map<Value, Document, ValueCmp> IdMap;

class CheckResultsBase : public Base {
public:
    virtual ~CheckResultsBase() {}
    void run() {
        runSharded(false);
        runSharded(true);
    }
    void runSharded(bool sharded) {
        createGroup(groupSpec());
        auto source = DocumentSourceMock::create(inputData());
        group()->setSource(source.get());

        intrusive_ptr<DocumentSource> sink = group();
        if (sharded) {
            sink = createMerger();
            // Serialize and re-parse the shard stage.
            createGroup(toBson(group())["$group"].Obj(), true);
            group()->setSource(source.get());
            sink->setSource(group());
        }

        checkResultSet(sink);
    }

protected:
    virtual deque<DocumentSource::GetNextResult> inputData() {
        return {};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << 0);
    }
    /** Expected results.  Must be sorted by _id to ensure consistent ordering. */
    virtual BSONObj expectedResultSet() {
        BSONObj wrappedResult =
            // fromjson cannot parse an array, so place the array within an object.
            fromjson(string("{'':") + expectedResultSetString() + "}");
        return wrappedResult[""].embeddedObject().getOwned();
    }
    /** Expected results.  Must be sorted by _id to ensure consistent ordering. */
    virtual string expectedResultSetString() {
        return "[]";
    }
    intrusive_ptr<DocumentSource> createMerger() {
        // Set up a group merger to simulate merging results in the router.  In this
        // case only one shard is in use.
        SplittableDocumentSource* splittable = dynamic_cast<SplittableDocumentSource*>(group());
        ASSERT(splittable);
        intrusive_ptr<DocumentSource> routerSource = splittable->getMergeSource();
        ASSERT_NOT_EQUALS(group(), routerSource.get());
        return routerSource;
    }
    void checkResultSet(const intrusive_ptr<DocumentSource>& sink) {
        // Load the results from the DocumentSourceGroup and sort them by _id.
        IdMap resultSet;
        for (auto output = sink->getNext(); output.isAdvanced(); output = sink->getNext()) {
            // Save the current result.
            Value id = output.getDocument().getField("_id");
            resultSet[id] = output.releaseDocument();
        }
        // Verify the DocumentSourceGroup is exhausted.
        assertEOF(sink);

        // Convert results to BSON once they all have been retrieved (to detect any errors
        // resulting from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (IdMap::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << i->second;
        }
        // Check the result set.
        ASSERT_BSONOBJ_EQ(expectedResultSet(), bsonResultSet.arr());
    }
};

/** An empty collection generates no results. */
class EmptyCollection : public CheckResultsBase {};

/** A $group performed on a single document. */
class SingleDocument : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("a" << 1)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << 0 << "a" << BSON("$sum"
                                              << "$a"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0,a:1}]";
    }
};

/** A $group performed on two values for a single key. */
class TwoValuesSingleKey : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("a" << 1), DOC("a" << 2)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << 0 << "a" << BSON("$push"
                                              << "$a"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0,a:[1,2]}]";
    }
};

/** A $group performed on two values with one key each. */
class TwoValuesTwoKeys : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("_id" << 0 << "a" << 1), DOC("_id" << 1 << "a" << 2)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id"
                    << "$_id"
                    << "a"
                    << BSON("$push"
                            << "$a"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0,a:[1]},{_id:1,a:[2]}]";
    }
};

/** A $group performed on two values with two keys each. */
class FourValuesTwoKeys : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("id" << 0 << "a" << 1),
                DOC("id" << 1 << "a" << 2),
                DOC("id" << 0 << "a" << 3),
                DOC("id" << 1 << "a" << 4)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id"
                    << "$id"
                    << "a"
                    << BSON("$push"
                            << "$a"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0,a:[1,3]},{_id:1,a:[2,4]}]";
    }
};

/** A $group performed on two values with two keys each and two accumulator operations. */
class FourValuesTwoKeysTwoAccumulators : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("id" << 0 << "a" << 1),
                DOC("id" << 1 << "a" << 2),
                DOC("id" << 0 << "a" << 3),
                DOC("id" << 1 << "a" << 4)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id"
                    << "$id"
                    << "list"
                    << BSON("$push"
                            << "$a")
                    << "sum"
                    << BSON("$sum" << BSON("$divide" << BSON_ARRAY("$a" << 2))));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0,list:[1,3],sum:2},{_id:1,list:[2,4],sum:3}]";
    }
};

/** Null and undefined _id values are grouped together. */
class GroupNullUndefinedIds : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("a" << BSONNULL << "b" << 100), DOC("b" << 10)};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id"
                    << "$a"
                    << "sum"
                    << BSON("$sum"
                            << "$b"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:null,sum:110}]";
    }
};

/** A complex _id expression. */
class ComplexId : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {DOC("a"
                    << "de"
                    << "b"
                    << "ad"
                    << "c"
                    << "beef"
                    << "d"
                    << ""),
                DOC("a"
                    << "d"
                    << "b"
                    << "eadbe"
                    << "c"
                    << ""
                    << "d"
                    << "ef")};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << BSON("$concat" << BSON_ARRAY("$a"
                                                          << "$b"
                                                          << "$c"
                                                          << "$d")));
    }
    virtual string expectedResultSetString() {
        return "[{_id:'deadbeef'}]";
    }
};

/** An undefined accumulator value is dropped. */
class UndefinedAccumulatorValue : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {Document()};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << 0 << "first" << BSON("$first"
                                                  << "$missing"));
    }
    virtual string expectedResultSetString() {
        return "[{_id:0, first:null}]";
    }
};

/** Simulate merging sharded results in the router. */
class RouterMerger : public CheckResultsBase {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{_id:0,list:[1,2]}",
                                                  "{_id:1,list:[3,4]}",
                                                  "{_id:0,list:[10,20]}",
                                                  "{_id:1,list:[30,40]}]}"});

        // Create a group source.
        createGroup(BSON("_id"
                         << "$x"
                         << "list"
                         << BSON("$push"
                                 << "$y")));
        // Create a merger version of the source.
        intrusive_ptr<DocumentSource> group = createMerger();
        // Attach the merger to the synthetic shard results.
        group->setSource(source.get());
        // Check the merger's output.
        checkResultSet(group);
    }

private:
    string expectedResultSetString() {
        return "[{_id:0,list:[1,2,10,20]},{_id:1,list:[3,4,30,40]}]";
    }
};

/** Dependant field paths. */
class Dependencies : public Base {
public:
    void run() {
        createGroup(fromjson("{_id:'$x',a:{$sum:'$y.z'},b:{$avg:{$add:['$u','$v']}}}"));
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, group()->getDependencies(&dependencies));
        ASSERT_EQUALS(4U, dependencies.fields.size());
        // Dependency from _id expression.
        ASSERT_EQUALS(1U, dependencies.fields.count("x"));
        // Dependencies from accumulator expressions.
        ASSERT_EQUALS(1U, dependencies.fields.count("y.z"));
        ASSERT_EQUALS(1U, dependencies.fields.count("u"));
        ASSERT_EQUALS(1U, dependencies.fields.count("v"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class StreamingOptimization : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 0}", "{a: 0}", "{a: 1}", "{a: 1}"});
        source->sorts = {BSON("a" << 1)};

        createGroup(BSON("_id"
                         << "$a"));
        group()->setSource(source.get());

        auto res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id"), Value(0));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("a"), Value(1));

        assertEOF(source);

        res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id"), Value(1));

        assertEOF(group());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 1U);

        ASSERT_EQUALS(outputSort.count(BSON("_id" << 1)), 1U);
    }
};

class StreamingWithMultipleIdFields : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create(
            {"{a: 1, b: 2}", "{a: 1, b: 2}", "{a: 1, b: 1}", "{a: 2, b: 1}", "{a: 2, b: 1}"});
        source->sorts = {BSON("a" << 1 << "b" << -1)};

        createGroup(fromjson("{_id: {x: '$a', y: '$b'}}"));
        group()->setSource(source.get());

        auto res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["x"], Value(1));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["y"], Value(2));

        ASSERT_TRUE(group()->isStreaming());

        res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["x"], Value(1));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["y"], Value(1));

        res = source->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("a"), Value(2));
        ASSERT_VALUE_EQ(res.getDocument().getField("b"), Value(1));

        assertEOF(source);

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 2U);

        BSONObj correctSort = BSON("_id.x" << 1 << "_id.y" << -1);
        ASSERT_EQUALS(outputSort.count(correctSort), 1U);

        BSONObj prefixSort = BSON("_id.x" << 1);
        ASSERT_EQUALS(outputSort.count(prefixSort), 1U);
    }
};

class StreamingWithMultipleLevels : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create(
            {"{a: {b: {c: 3}}, d: 1}", "{a: {b: {c: 1}}, d: 2}", "{a: {b: {c: 1}}, d: 0}"});
        source->sorts = {BSON("a.b.c" << -1 << "a.b.d" << 1 << "d" << 1)};

        createGroup(fromjson("{_id: {x: {y: {z: '$a.b.c', q: '$a.b.d'}}, v: '$d'}}"));
        group()->setSource(source.get());

        auto res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["x"]["y"]["z"], Value(3));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("a")["b"]["c"], Value(1));

        assertEOF(source);

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 3U);

        BSONObj correctSort = fromjson("{'_id.x.y.z': -1, '_id.x.y.q': 1, '_id.v': 1}");
        ASSERT_EQUALS(outputSort.count(correctSort), 1U);

        BSONObj prefixSortTwo = fromjson("{'_id.x.y.z': -1, '_id.x.y.q': 1}");
        ASSERT_EQUALS(outputSort.count(prefixSortTwo), 1U);

        BSONObj prefixSortOne = fromjson("{'_id.x.y.z': -1}");
        ASSERT_EQUALS(outputSort.count(prefixSortOne), 1U);
    }
};

class StreamingWithFieldRepeated : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create(
            {"{a: 1, b: 1}", "{a: 1, b: 1}", "{a: 2, b: 1}", "{a: 2, b: 3}"});
        source->sorts = {BSON("a" << 1 << "b" << 1)};

        createGroup(fromjson("{_id: {sub: {x: '$a', y: '$b', z: '$a'}}}"));
        group()->setSource(source.get());

        auto res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["x"], Value(1));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["y"], Value(1));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["z"], Value(1));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("a"), Value(2));
        ASSERT_VALUE_EQ(res.getDocument().getField("b"), Value(3));

        BSONObjSet outputSort = group()->getOutputSorts();

        ASSERT_EQUALS(outputSort.size(), 2U);

        BSONObj correctSort = fromjson("{'_id.sub.z': 1}");
        ASSERT_EQUALS(outputSort.count(correctSort), 1U);

        BSONObj prefixSortTwo = fromjson("{'_id.sub.z': 1, '_id.sub.y': 1}");
        ASSERT_EQUALS(outputSort.count(prefixSortTwo), 1U);
    }
};

class StreamingWithConstantAndFieldPath : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create(
            {"{a: 5, b: 1}", "{a: 5, b: 2}", "{a: 3, b: 1}", "{a: 1, b: 1}", "{a: 1, b: 1}"});
        source->sorts = {BSON("a" << -1 << "b" << 1)};

        createGroup(fromjson("{_id: {sub: {x: '$a', y: '$b', z: {$literal: 'c'}}}}"));
        group()->setSource(source.get());

        auto res = group()->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["x"], Value(5));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["y"], Value(1));
        ASSERT_VALUE_EQ(res.getDocument().getField("_id")["sub"]["z"], Value("c"));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(res.isAdvanced());
        ASSERT_VALUE_EQ(res.getDocument().getField("a"), Value(3));
        ASSERT_VALUE_EQ(res.getDocument().getField("b"), Value(1));

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 2U);

        BSONObj correctSort = fromjson("{'_id.sub.x': -1}");
        ASSERT_EQUALS(outputSort.count(correctSort), 1U);

        BSONObj prefixSortTwo = fromjson("{'_id.sub.x': -1, '_id.sub.y': 1}");
        ASSERT_EQUALS(outputSort.count(prefixSortTwo), 1U);
    }
};

class StreamingWithRootSubfield : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}", "{a: 3}"});
        source->sorts = {BSON("a" << 1)};

        createGroup(fromjson("{_id: '$$ROOT.a'}"));
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_TRUE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 1U);

        BSONObj correctSort = fromjson("{_id: 1}");
        ASSERT_EQUALS(outputSort.count(correctSort), 1U);
    }
};

class StreamingWithConstant : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}", "{a: 3}"});
        source->sorts = {BSON("$a" << 1)};

        createGroup(fromjson("{_id: 1}"));
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_TRUE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 0U);
    }
};

class StreamingWithEmptyId : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}", "{a: 3}"});
        source->sorts = {BSON("$a" << 1)};

        createGroup(fromjson("{_id: {}}"));
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_TRUE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 0U);
    }
};

class NoOptimizationIfMissingDoubleSort : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}", "{a: 3}"});
        source->sorts = {BSON("a" << 1)};

        // We pretend to be in the router so that we don't spill to disk, because this produces
        // inconsistent output on debug vs. non-debug builds.
        const bool inRouter = true;
        const bool inShard = false;

        createGroup(BSON("_id" << BSON("x"
                                       << "$a"
                                       << "y"
                                       << "$b")),
                    inShard,
                    inRouter);
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_FALSE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 0U);
    }
};

class NoOptimizationWithRawRoot : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}", "{a: 3}"});
        source->sorts = {BSON("a" << 1)};

        // We pretend to be in the router so that we don't spill to disk, because this produces
        // inconsistent output on debug vs. non-debug builds.
        const bool inRouter = true;
        const bool inShard = false;

        createGroup(BSON("_id" << BSON("a"
                                       << "$$ROOT"
                                       << "b"
                                       << "$a")),
                    inShard,
                    inRouter);
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_FALSE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 0U);
    }
};

class NoOptimizationIfUsingExpressions : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1, b: 1}", "{a: 2, b: 2}", "{a: 3, b: 1}"});
        source->sorts = {BSON("a" << 1 << "b" << 1)};

        // We pretend to be in the router so that we don't spill to disk, because this produces
        // inconsistent output on debug vs. non-debug builds.
        const bool inRouter = true;
        const bool inShard = false;

        createGroup(fromjson("{_id: {$sum: ['$a', '$b']}}"), inShard, inRouter);
        group()->setSource(source.get());

        group()->getNext();
        ASSERT_FALSE(group()->isStreaming());

        BSONObjSet outputSort = group()->getOutputSorts();
        ASSERT_EQUALS(outputSort.size(), 0U);
    }
};

/**
 * A string constant (not a field path) as an _id expression and passed to an accumulator.
 * SERVER-6766
 */
class StringConstantIdAndAccumulatorExpressions : public CheckResultsBase {
    deque<DocumentSource::GetNextResult> inputData() {
        return {Document()};
    }
    BSONObj groupSpec() {
        return fromjson("{_id:{$const:'$_id...'},a:{$push:{$const:'$a...'}}}");
    }
    string expectedResultSetString() {
        return "[{_id:'$_id...',a:['$a...']}]";
    }
};

/** An array constant passed to an accumulator. */
class ArrayConstantAccumulatorExpression : public CheckResultsBase {
public:
    void run() {
        // A parse exception is thrown when a raw array is provided to an accumulator.
        ASSERT_THROWS(createGroup(fromjson("{_id:1,a:{$push:[4,5,6]}}")), UserException);
        // Run standard base tests.
        CheckResultsBase::run();
    }
    deque<DocumentSource::GetNextResult> inputData() {
        return {Document()};
    }
    BSONObj groupSpec() {
        // An array can be specified using $const.
        return fromjson("{_id:[1,2,3],a:{$push:{$const:[4,5,6]}}}");
    }
    string expectedResultSetString() {
        return "[{_id:[1,2,3],a:[[4,5,6]]}]";
    }
};

class All : public Suite {
public:
    All() : Suite("DocumentSourceGroupTests") {}
    void setupTests() {
        add<NonObject>();
        add<EmptySpec>();
        add<IdEmptyObject>();
        add<IdObjectExpression>();
        add<IdInvalidObjectExpression>();
        add<TwoIdSpecs>();
        add<IdEmptyString>();
        add<IdStringConstant>();
        add<IdFieldPath>();
        add<IdInvalidFieldPath>();
        add<IdNumericConstant>();
        add<IdArrayConstant>();
        add<IdRegularExpression>();
        add<DollarAggregateFieldName>();
        add<NonObjectAggregateSpec>();
        add<EmptyObjectAggregateSpec>();
        add<BadAccumulator>();
        add<SumArray>();
        add<MultipleAccumulatorsForAField>();
        add<DuplicateAggregateFieldNames>();
        add<AggregateObjectExpression>();
        add<AggregateOperatorExpression>();
        add<EmptyCollection>();
        add<SingleDocument>();
        add<TwoValuesSingleKey>();
        add<TwoValuesTwoKeys>();
        add<FourValuesTwoKeys>();
        add<FourValuesTwoKeysTwoAccumulators>();
        add<GroupNullUndefinedIds>();
        add<ComplexId>();
        add<UndefinedAccumulatorValue>();
        add<RouterMerger>();
        add<Dependencies>();
        add<StringConstantIdAndAccumulatorExpressions>();
        add<ArrayConstantAccumulatorExpression>();
#if 0
        // Disabled tests until SERVER-23318 is implemented.
        add<StreamingOptimization>();
        add<StreamingWithMultipleIdFields>();
        add<NoOptimizationIfMissingDoubleSort>();
        add<NoOptimizationWithRawRoot>();
        add<NoOptimizationIfUsingExpressions>();
        add<StreamingWithMultipleLevels>();
        add<StreamingWithConstant>();
        add<StreamingWithEmptyId>();
        add<StreamingWithRootSubfield>();
        add<StreamingWithConstantAndFieldPath>();
        add<StreamingWithFieldRepeated>();
#endif
    }
};

SuiteInstance<All> myall;

}  // namespace
}  // namespace mongo
