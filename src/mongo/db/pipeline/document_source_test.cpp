/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#include "mongo/base/init.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
bool isMongos() {
    return false;
}

std::unique_ptr<ServiceContextNoop> makeTestServiceContext() {
    auto service = stdx::make_unique<ServiceContextNoop>();
    service->setFastClockSource(stdx::make_unique<ClockSourceMock>());
    service->setTickSource(stdx::make_unique<TickSourceMock>());
    return service;
}
}

// Stub to avoid including the server environment library.
MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(makeTestServiceContext());
    return Status::OK();
}

namespace DocumentSourceTests {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::map;
using std::set;
using std::string;
using std::vector;

static const char* const ns = "unittests.documentsourcetests";
static const BSONObj metaTextScore = BSON("$meta"
                                          << "textScore");

BSONObj toBson(const intrusive_ptr<DocumentSource>& source) {
    vector<Value> arr;
    source->serializeToArray(arr);
    ASSERT_EQUALS(arr.size(), 1UL);
    return arr[0].getDocument().toBson();
}


namespace DocumentSourceClass {
using mongo::DocumentSource;

TEST(TruncateSort, SortTruncatesNormalField) {
    BSONObj sortKey = BSON("a" << 1 << "b" << 1 << "c" << 1);
    auto truncated = DocumentSource::truncateSortSet({sortKey}, {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

TEST(TruncateSort, SortTruncatesOnSubfield) {
    BSONObj sortKey = BSON("a" << 1 << "b.c" << 1 << "d" << 1);
    auto truncated = DocumentSource::truncateSortSet({sortKey}, {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

TEST(TruncateSort, SortDoesNotTruncateOnParent) {
    BSONObj sortKey = BSON("a" << 1 << "b" << 1 << "d" << 1);
    auto truncated = DocumentSource::truncateSortSet({sortKey}, {"b.c"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1 << "b" << 1 << "d" << 1)), 1U);
}

TEST(TruncateSort, TruncateSortDedupsSortCorrectly) {
    BSONObj sortKeyOne = BSON("a" << 1 << "b" << 1);
    BSONObj sortKeyTwo = BSON("a" << 1);
    auto truncated = DocumentSource::truncateSortSet({sortKeyOne, sortKeyTwo}, {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

template <size_t ArrayLen>
set<string> arrayToSet(const char* (&array)[ArrayLen]) {
    set<string> out;
    for (size_t i = 0; i < ArrayLen; i++)
        out.insert(array[i]);
    return out;
}

class Deps {
public:
    void run() {
        {
            const char* array[] = {"a", "b"};  // basic
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
        }
        {
            const char* array[] = {"a", "ab"};  // prefixed but not subfield
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "ab" << 1 << "_id" << 0));
        }
        {
            const char* array[] = {"a", "b", "a.b"};  // a.b included by a
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
        }
        {
            const char* array[] = {"a", "_id"};  // _id now included
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "_id" << 1));
        }
        {
            const char* array[] = {"a", "_id.a"};  // still include whole _id (SERVER-7502)
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "_id" << 1));
        }
        {
            const char* array[] = {"a", "_id", "_id.a"};  // handle both _id and subfield
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("a" << 1 << "_id" << 1));
        }
        {
            const char* array[] = {"a", "_id", "_id_a"};  // _id prefixed but non-subfield
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            ASSERT_EQUALS(deps.toProjection(), BSON("_id_a" << 1 << "a" << 1 << "_id" << 1));
        }
        {
            const char* array[] = {"a"};  // fields ignored with needWholeDocument
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            deps.needWholeDocument = true;
            ASSERT_EQUALS(deps.toProjection(), BSONObj());
        }
        {
            const char* array[] = {"a"};  // needTextScore with needWholeDocument
            DepsTracker deps(DepsTracker::MetadataAvailable::kTextScore);
            deps.fields = arrayToSet(array);
            deps.needWholeDocument = true;
            deps.setNeedTextScore(true);
            ASSERT_EQUALS(deps.toProjection(), BSON(Document::metaFieldTextScore << metaTextScore));
        }
        {
            const char* array[] = {"a"};  // needTextScore without needWholeDocument
            DepsTracker deps(DepsTracker::MetadataAvailable::kTextScore);
            deps.fields = arrayToSet(array);
            deps.setNeedTextScore(true);
            ASSERT_EQUALS(
                deps.toProjection(),
                BSON(Document::metaFieldTextScore << metaTextScore << "a" << 1 << "_id" << 0));
        }
    }
};


}  // namespace DocumentSourceClass

namespace Mock {
using mongo::DocumentSourceMock;

/**
 * A fixture which provides access to things like a ServiceContext that are needed by other tests.
 */
class Base {
public:
    Base()
        : _service(makeTestServiceContext()),
          _client(_service->makeClient("DocumentSourceTest")),
          _opCtx(_client->makeOperationContext()),
          _ctx(new ExpressionContext(_opCtx.get(), AggregationRequest(NamespaceString(ns), {}))) {}

protected:
    intrusive_ptr<ExpressionContext> ctx() {
        return _ctx;
    }

    std::unique_ptr<ServiceContextNoop> _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    intrusive_ptr<ExpressionContext> _ctx;
};

TEST(Mock, OneDoc) {
    auto doc = DOC("a" << 1);
    auto source = DocumentSourceMock::create(doc);
    ASSERT_DOCUMENT_EQ(*source->getNext(), doc);
    ASSERT(!source->getNext());
}

TEST(Mock, DequeDocuments) {
    auto source = DocumentSourceMock::create({DOC("a" << 1), DOC("a" << 2)});
    ASSERT_DOCUMENT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT_DOCUMENT_EQ(*source->getNext(), DOC("a" << 2));
    ASSERT(!source->getNext());
}

TEST(Mock, StringJSON) {
    auto source = DocumentSourceMock::create("{a : 1}");
    ASSERT_DOCUMENT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT(!source->getNext());
}

TEST(Mock, DequeStringJSONs) {
    auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}"});
    ASSERT_DOCUMENT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT_DOCUMENT_EQ(*source->getNext(), DOC("a" << 2));
    ASSERT(!source->getNext());
}

TEST(Mock, Empty) {
    auto source = DocumentSourceMock::create();
    ASSERT(!source->getNext());
}

}  // namespace Mock

namespace DocumentSourceRedact {
using mongo::DocumentSourceRedact;
using mongo::DocumentSourceMatch;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
protected:
    void createRedact() {
        BSONObj spec = BSON("$redact"
                            << "$$PRUNE");
        _redact = DocumentSourceRedact::createFromBson(spec.firstElement(), ctx());
    }

    DocumentSource* redact() {
        return _redact.get();
    }

private:
    intrusive_ptr<DocumentSource> _redact;
};

class PromoteMatch : public Base {
public:
    void run() {
        createRedact();

        auto match = DocumentSourceMatch::createFromBson(BSON("a" << 1).firstElement(), ctx());

        Pipeline::SourceContainer pipeline;
        pipeline.push_back(redact());
        pipeline.push_back(match);

        pipeline.front()->optimizeAt(pipeline.begin(), &pipeline);

        ASSERT_EQUALS(pipeline.size(), 4U);
        ASSERT(dynamic_cast<DocumentSourceMatch*>(pipeline.front().get()));
    }
};
}  // namespace DocumentSourceRedact

namespace DocumentSourceLimit {

using mongo::DocumentSourceLimit;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
protected:
    void createLimit(int limit) {
        BSONObj spec = BSON("$limit" << limit);
        BSONElement specElement = spec.firstElement();
        _limit = DocumentSourceLimit::createFromBson(specElement, ctx());
    }
    DocumentSource* limit() {
        return _limit.get();
    }

private:
    intrusive_ptr<DocumentSource> _limit;
};

/** Exhausting a DocumentSourceLimit disposes of the limit's source. */
class DisposeSource : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}"});
        createLimit(1);
        limit()->setSource(source.get());
        // The limit's result is as expected.
        boost::optional<Document> next = limit()->getNext();
        ASSERT(bool(next));
        ASSERT_VALUE_EQ(Value(1), next->getField("a"));
        // The limit is exhausted.
        ASSERT(!limit()->getNext());
    }
};

/** Combine two $limit stages. */
class CombineLimit : public Base {
public:
    void run() {
        Pipeline::SourceContainer container;
        createLimit(10);

        auto secondLimit =
            DocumentSourceLimit::createFromBson(BSON("$limit" << 5).firstElement(), ctx());

        container.push_back(limit());
        container.push_back(secondLimit);

        limit()->optimizeAt(container.begin(), &container);
        ASSERT_EQUALS(5, static_cast<DocumentSourceLimit*>(limit())->getLimit());
        ASSERT_EQUALS(1U, container.size());
    }
};

/** Exhausting a DocumentSourceLimit disposes of the pipeline's source. */
class DisposeSourceCascade : public Base {
public:
    void run() {
        auto source = DocumentSourceMock::create({"{a: 1}", "{a: 1}"});
        // Create a DocumentSourceMatch.
        BSONObj spec = BSON("$match" << BSON("a" << 1));
        BSONElement specElement = spec.firstElement();
        intrusive_ptr<DocumentSource> match =
            DocumentSourceMatch::createFromBson(specElement, ctx());
        match->setSource(source.get());

        createLimit(1);
        limit()->setSource(match.get());
        // The limit is not exhauted.
        boost::optional<Document> next = limit()->getNext();
        ASSERT(bool(next));
        ASSERT_VALUE_EQ(Value(1), next->getField("a"));
        // The limit is exhausted.
        ASSERT(!limit()->getNext());
    }
};

/** A limit does not introduce any dependencies. */
class Dependencies : public Base {
public:
    void run() {
        createLimit(1);
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, limit()->getDependencies(&dependencies));
        ASSERT_EQUALS(0U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

}  // namespace DocumentSourceLimit

namespace DocumentSourceLookup {

TEST(QueryForInput, NonArrayValueUsesEqQuery) {
    Document input = DOC("local" << 1);
    BSONObj query =
        DocumentSourceLookUp::queryForInput(input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_EQ(query, fromjson("{$and: [{foreign: {$eq: 1}}, {}]}"));
}

TEST(QueryForInput, RegexValueUsesEqQuery) {
    BSONRegEx regex("^a");
    Document input = DOC("local" << Value(regex));
    BSONObj query =
        DocumentSourceLookUp::queryForInput(input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_EQ(query,
              BSON("$and" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << regex)) << BSONObj())));
}

TEST(QueryForInput, ArrayValueUsesInQuery) {
    vector<Value> inputArray = {Value(1), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj query =
        DocumentSourceLookUp::queryForInput(input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_EQ(query, fromjson("{$and: [{foreign: {$in: [1, 2]}}, {}]}"));
}

TEST(QueryForInput, ArrayValueWithRegexUsesOrQuery) {
    BSONRegEx regex("^a");
    vector<Value> inputArray = {Value(1), Value(regex), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj query =
        DocumentSourceLookUp::queryForInput(input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_EQ(query,
              BSON("$and" << BSON_ARRAY(
                       BSON("$or" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << Value(1)))
                                                << BSON("foreign" << BSON("$eq" << regex))
                                                << BSON("foreign" << BSON("$eq" << Value(2)))))
                       << BSONObj())));
}

}  // namespace DocumentSourceLookUp

namespace DocumentSourceGroup {

using mongo::DocumentSourceGroup;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
public:
    Base() : _tempDir("DocumentSourceGroupTest") {}

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
    void assertExhausted(const intrusive_ptr<DocumentSource>& source) const {
        // It should be safe to check doneness multiple times
        ASSERT(!source->getNext());
        ASSERT(!source->getNext());
        ASSERT(!source->getNext());
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
        ASSERT_EQUALS(spec, toBson(generated));
    }
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
        boost::optional<Document> next = group()->getNext();
        ASSERT(bool(next));
        // The constant _id value from the $group spec is passed through.
        ASSERT_EQUALS(expected(), next->toBson());
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
        return Value::compare(a, b) < 0;
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
    virtual std::deque<Document> inputData() {
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
        while (boost::optional<Document> current = sink->getNext()) {
            // Save the current result.
            Value id = current->getField("_id");
            resultSet[id] = *current;
        }
        // Verify the DocumentSourceGroup is exhausted.
        assertExhausted(sink);

        // Convert results to BSON once they all have been retrieved (to detect any errors
        // resulting from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (IdMap::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << i->second;
        }
        // Check the result set.
        ASSERT_EQUALS(expectedResultSet(), bsonResultSet.arr());
    }
};

/** An empty collection generates no results. */
class EmptyCollection : public CheckResultsBase {};

/** A $group performed on a single document. */
class SingleDocument : public CheckResultsBase {
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id"), Value(0));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("a"), Value(1));

        assertExhausted(source);

        res = group()->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id"), Value(1));

        assertExhausted(group());

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
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id")["x"], Value(1));
        ASSERT_VALUE_EQ(res->getField("_id")["y"], Value(2));

        ASSERT_TRUE(group()->isStreaming());

        res = group()->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id")["x"], Value(1));
        ASSERT_VALUE_EQ(res->getField("_id")["y"], Value(1));

        res = source->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("a"), Value(2));
        ASSERT_VALUE_EQ(res->getField("b"), Value(1));

        assertExhausted(source);

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
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id")["x"]["y"]["z"], Value(3));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("a")["b"]["c"], Value(1));

        assertExhausted(source);

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
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["x"], Value(1));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["y"], Value(1));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["z"], Value(1));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("a"), Value(2));
        ASSERT_VALUE_EQ(res->getField("b"), Value(3));

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
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["x"], Value(5));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["y"], Value(1));
        ASSERT_VALUE_EQ(res->getField("_id")["sub"]["z"], Value("c"));

        ASSERT_TRUE(group()->isStreaming());

        res = source->getNext();
        ASSERT_TRUE(bool(res));
        ASSERT_VALUE_EQ(res->getField("a"), Value(3));
        ASSERT_VALUE_EQ(res->getField("b"), Value(1));

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
    std::deque<Document> inputData() {
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
    std::deque<Document> inputData() {
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

}  // namespace DocumentSourceGroup

namespace DocumentSourceProject {

using mongo::DocumentSourceMock;
using mongo::DocumentSourceProject;

//
// DocumentSourceProject delegates much of its responsibilities to the ParsedAggregationProjection.
// Most of the functional tests are testing ParsedAggregationProjection directly. These are meant as
// simpler integration tests.
//

/**
 * Class which provides useful helpers to test the functionality of the $project stage.
 */
class ProjectStageTest : public Mock::Base, public unittest::Test {
protected:
    /**
     * Creates the $project stage, which can be accessed via project().
     */
    void createProject(const BSONObj& projection) {
        BSONObj spec = BSON("$project" << projection);
        BSONElement specElement = spec.firstElement();
        _project = DocumentSourceProject::createFromBson(specElement, ctx());
    }

    DocumentSource* project() {
        return _project.get();
    }

    /**
     * Assert that iterator state accessors consistently report the source is exhausted.
     */
    void assertExhausted() const {
        ASSERT(!_project->getNext());
        ASSERT(!_project->getNext());
        ASSERT(!_project->getNext());
    }

private:
    intrusive_ptr<DocumentSource> _project;
};

TEST_F(ProjectStageTest, InclusionProjectionShouldRemoveUnspecifiedFields) {
    createProject(BSON("a" << true << "c" << BSON("d" << true)));
    auto source = DocumentSourceMock::create("{_id: 0, a: 1, b: 1, c: {d: 1}}");
    project()->setSource(source.get());
    // The first result exists and is as expected.
    boost::optional<Document> next = project()->getNext();
    ASSERT_TRUE(next);
    ASSERT_EQUALS(1, next->getField("a").getInt());
    ASSERT(next->getField("b").missing());
    // The _id field is included by default in the root document.
    ASSERT_EQUALS(0, next->getField("_id").getInt());
    // The nested c.d inclusion.
    ASSERT_EQUALS(1, (*next)["c"]["d"].getInt());
};

TEST_F(ProjectStageTest, ShouldOptimizeInnerExpressions) {
    createProject(BSON("a" << BSON("$and" << BSON_ARRAY(BSON("$const" << true)))));
    project()->optimize();
    // The $and should have been replaced with its only argument.
    vector<Value> serializedArray;
    project()->serializeToArray(serializedArray);
    ASSERT_EQUALS(serializedArray[0].getDocument().toBson(),
                  fromjson("{$project: {_id: true, a: {$const: true}}}"));
};

TEST_F(ProjectStageTest, ShouldErrorOnNonObjectSpec) {
    // Can't use createProject() helper because we want to give a non-object spec.
    BSONObj spec = BSON("$project"
                        << "foo");
    BSONElement specElement = spec.firstElement();
    ASSERT_THROWS(DocumentSourceProject::createFromBson(specElement, ctx()), UserException);
};

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, InclusionShouldBeAbleToProcessMultipleDocuments) {
    createProject(BSON("a" << true));
    auto source = DocumentSourceMock::create({"{a: 1, b: 2}", "{a: 3, b: 4}"});
    project()->setSource(source.get());
    boost::optional<Document> next = project()->getNext();
    ASSERT(bool(next));
    ASSERT_EQUALS(1, next->getField("a").getInt());
    ASSERT(next->getField("b").missing());

    next = project()->getNext();
    ASSERT(bool(next));
    ASSERT_EQUALS(3, next->getField("a").getInt());
    ASSERT(next->getField("b").missing());

    assertExhausted();
};

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, ExclusionShouldBeAbleToProcessMultipleDocuments) {
    createProject(BSON("a" << false));
    auto source = DocumentSourceMock::create({"{a: 1, b: 2}", "{a: 3, b: 4}"});
    project()->setSource(source.get());
    boost::optional<Document> next = project()->getNext();
    ASSERT(bool(next));
    ASSERT(next->getField("a").missing());
    ASSERT_EQUALS(2, next->getField("b").getInt());

    next = project()->getNext();
    ASSERT(bool(next));
    ASSERT(next->getField("a").missing());
    ASSERT_EQUALS(4, next->getField("b").getInt());

    assertExhausted();
};

TEST_F(ProjectStageTest, InclusionShouldAddDependenciesOfIncludedAndComputedFields) {
    createProject(fromjson("{a: true, x: '$b', y: {$and: ['$c','$d']}, z: {$meta: 'textScore'}}"));
    DepsTracker dependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_FIELDS, project()->getDependencies(&dependencies));
    ASSERT_EQUALS(5U, dependencies.fields.size());

    // Implicit _id dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("_id"));

    // Inclusion dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));

    // Field path expression dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));

    // Nested expression dependencies.
    ASSERT_EQUALS(1U, dependencies.fields.count("c"));
    ASSERT_EQUALS(1U, dependencies.fields.count("d"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedTextScore());
};

TEST_F(ProjectStageTest, ExclusionShouldNotAddDependencies) {
    createProject(fromjson("{a: false, 'b.c': false}"));

    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, project()->getDependencies(&dependencies));

    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
};

}  // namespace DocumentSourceProject

namespace DocumentSourceSample {

using mongo::DocumentSourceSample;
using mongo::DocumentSourceMock;

class SampleBasics : public Mock::Base, public unittest::Test {
public:
    SampleBasics() : _mock(DocumentSourceMock::create()) {}

protected:
    virtual void createSample(long long size) {
        BSONObj spec = BSON("$sample" << BSON("size" << size));
        BSONElement specElement = spec.firstElement();
        _sample = DocumentSourceSample::createFromBson(specElement, ctx());
        sample()->setSource(_mock.get());
        checkBsonRepresentation(spec);
    }

    DocumentSource* sample() {
        return _sample.get();
    }

    DocumentSourceMock* source() {
        return _mock.get();
    }

    /**
     * Makes some general assertions about the results of a $sample stage.
     *
     * Creates a $sample stage with the given size, advances it 'nExpectedResults' times, asserting
     * the results come back in sorted order according to their assigned random values, then asserts
     * the stage is exhausted.
     */
    void checkResults(long long size, long long nExpectedResults) {
        createSample(size);

        boost::optional<Document> prevDoc;
        for (long long i = 0; i < nExpectedResults; i++) {
            auto thisDoc = sample()->getNext();
            ASSERT_TRUE(bool(thisDoc));
            ASSERT_TRUE(thisDoc->hasRandMetaField());
            if (prevDoc) {
                ASSERT_LTE(thisDoc->getRandMetaField(), prevDoc->getRandMetaField());
            }
            prevDoc = std::move(thisDoc);
        }
        assertExhausted();
    }

    /**
     * Helper to load 'nDocs' documents into the source stage.
     */
    void loadDocuments(int nDocs) {
        for (int i = 0; i < nDocs; i++) {
            _mock->queue.push_back(DOC("_id" << i));
        }
    }

    /**
     * Assert that iterator state accessors consistently report the source is exhausted.
     */
    void assertExhausted() const {
        ASSERT(!_sample->getNext());
        ASSERT(!_sample->getNext());
        ASSERT(!_sample->getNext());
    }

protected:
    intrusive_ptr<DocumentSource> _sample;
    intrusive_ptr<DocumentSourceMock> _mock;

private:
    /**
     * Check that the BSON representation generated by the souce matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        Value serialized = static_cast<DocumentSourceSample*>(sample())->serialize(false);
        auto generatedSpec = serialized.getDocument().toBson();
        ASSERT_EQUALS(spec, generatedSpec);
    }
};

/**
 * A sample of size 0 should return 0 results.
 */
TEST_F(SampleBasics, ZeroSize) {
    loadDocuments(2);
    checkResults(0, 0);
}

/**
 * If the source stage is exhausted, the $sample stage should also be exhausted.
 */
TEST_F(SampleBasics, SourceExhaustedBeforeSample) {
    loadDocuments(5);
    checkResults(10, 5);
}

/**
 * A $sample stage should limit the number of results to the given size.
 */
TEST_F(SampleBasics, SampleExhaustedBeforeSource) {
    loadDocuments(10);
    checkResults(5, 5);
}

/**
 * The incoming documents should not be modified by a $sample stage (except their metadata).
 */
TEST_F(SampleBasics, DocsUnmodified) {
    createSample(1);
    source()->queue.push_back(DOC("a" << 1 << "b" << DOC("c" << 2)));
    auto next = sample()->getNext();
    ASSERT_TRUE(bool(next));
    Document doc = *next;
    ASSERT_EQUALS(1, doc["a"].getInt());
    ASSERT_EQUALS(2, doc["b"]["c"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    assertExhausted();
}

/**
 * Fixture to test error cases of the $sample stage.
 */
class InvalidSampleSpec : public Mock::Base, public unittest::Test {
public:
    intrusive_ptr<DocumentSource> createSample(BSONObj sampleSpec) {
        auto specElem = sampleSpec.firstElement();
        return DocumentSourceSample::createFromBson(specElem, ctx());
    }

    BSONObj createSpec(BSONObj spec) {
        return BSON("$sample" << spec);
    }
};

TEST_F(InvalidSampleSpec, NonObject) {
    ASSERT_THROWS_CODE(createSample(BSON("$sample" << 1)), UserException, 28745);
    ASSERT_THROWS_CODE(createSample(BSON("$sample"
                                         << "string")),
                       UserException,
                       28745);
}

TEST_F(InvalidSampleSpec, NonNumericSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size"
                                                    << "string"))),
                       UserException,
                       28746);
}

TEST_F(InvalidSampleSpec, NegativeSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1))), UserException, 28747);
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1.0))), UserException, 28747);
}

TEST_F(InvalidSampleSpec, ExtraOption) {
    ASSERT_THROWS_CODE(
        createSample(createSpec(BSON("size" << 1 << "extra" << 2))), UserException, 28748);
}

TEST_F(InvalidSampleSpec, MissingSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSONObj())), UserException, 28749);
}

namespace DocumentSourceSampleFromRandomCursor {
using mongo::DocumentSourceSampleFromRandomCursor;

class SampleFromRandomCursorBasics : public SampleBasics {
public:
    void createSample(long long size) override {
        _sample = DocumentSourceSampleFromRandomCursor::create(ctx(), size, "_id", 100);
        sample()->setSource(_mock.get());
    }
};

/**
 * A sample of size zero should not return any results.
 */
TEST_F(SampleFromRandomCursorBasics, ZeroSize) {
    loadDocuments(2);
    checkResults(0, 0);
}

/**
 * When sampling with a size smaller than the number of documents our source stage can produce,
 * there should be no more than the sample size output.
 */
TEST_F(SampleFromRandomCursorBasics, SourceExhaustedBeforeSample) {
    loadDocuments(5);
    checkResults(10, 5);
}

/**
 * When the source stage runs out of documents, the $sampleFromRandomCursors stage should be
 * exhausted.
 */
TEST_F(SampleFromRandomCursorBasics, SampleExhaustedBeforeSource) {
    loadDocuments(10);
    checkResults(5, 5);
}

/**
 * The $sampleFromRandomCursor stage should not modify the contents of the documents.
 */
TEST_F(SampleFromRandomCursorBasics, DocsUnmodified) {
    createSample(1);
    source()->queue.push_back(DOC("_id" << 1 << "b" << DOC("c" << 2)));
    auto next = sample()->getNext();
    ASSERT_TRUE(bool(next));
    Document doc = *next;
    ASSERT_EQUALS(1, doc["_id"].getInt());
    ASSERT_EQUALS(2, doc["b"]["c"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    assertExhausted();
}

/**
 * The $sampleFromRandomCursor stage should ignore duplicate documents.
 */
TEST_F(SampleFromRandomCursorBasics, IgnoreDuplicates) {
    createSample(2);
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("_id" << 1));  // Duplicate, should ignore.
    source()->queue.push_back(DOC("_id" << 2));

    auto next = sample()->getNext();
    ASSERT_TRUE(bool(next));
    Document doc = *next;
    ASSERT_EQUALS(1, doc["_id"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    double doc1Meta = doc.getRandMetaField();

    // Should ignore the duplicate {_id: 1}, and return {_id: 2}.
    next = sample()->getNext();
    ASSERT_TRUE(bool(next));
    doc = *next;
    ASSERT_EQUALS(2, doc["_id"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    double doc2Meta = doc.getRandMetaField();
    ASSERT_GTE(doc1Meta, doc2Meta);

    // Both stages should be exhausted.
    ASSERT_FALSE(source()->getNext());
    assertExhausted();
}

/**
 * The $sampleFromRandomCursor stage should error if it receives too many duplicate documents.
 */
TEST_F(SampleFromRandomCursorBasics, TooManyDups) {
    createSample(2);
    for (int i = 0; i < 1000; i++) {
        source()->queue.push_back(DOC("_id" << 1));
    }

    // First should be successful, it's not a duplicate.
    ASSERT_TRUE(bool(sample()->getNext()));

    // The rest are duplicates, should error.
    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28799);
}

/**
 * The $sampleFromRandomCursor stage should error if it receives a document without an _id.
 */
TEST_F(SampleFromRandomCursorBasics, MissingIdField) {
    // Once with only a bad document.
    createSample(2);  // _idField is '_id'.
    source()->queue.push_back(DOC("non_id" << 2));
    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28793);

    // Again, with some regular documents before a bad one.
    createSample(2);  // _idField is '_id'.
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("non_id" << 2));

    // First should be successful.
    ASSERT_TRUE(bool(sample()->getNext()));

    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28793);
}

/**
 * The $sampleFromRandomCursor stage should set the random meta value in a way that mimics the
 * non-optimized case.
 */
TEST_F(SampleFromRandomCursorBasics, MimicNonOptimized) {
    // Compute the average random meta value on the each doc returned.
    double firstTotal = 0.0;
    double secondTotal = 0.0;
    int nTrials = 10000;
    for (int i = 0; i < nTrials; i++) {
        // Sample 2 out of 3 documents.
        _sample = DocumentSourceSampleFromRandomCursor::create(ctx(), 2, "_id", 3);
        sample()->setSource(_mock.get());

        source()->queue.push_back(DOC("_id" << 1));
        source()->queue.push_back(DOC("_id" << 2));

        auto doc = sample()->getNext();
        ASSERT_TRUE(bool(doc));
        ASSERT_TRUE((*doc).hasRandMetaField());
        firstTotal += (*doc).getRandMetaField();

        doc = sample()->getNext();
        ASSERT_TRUE(bool(doc));
        ASSERT_TRUE((*doc).hasRandMetaField());
        secondTotal += (*doc).getRandMetaField();
    }
    // The average random meta value of the first document should be about 0.75. We assume that
    // 10000 trials is sufficient for us to apply the Central Limit Theorem. Using an error
    // tolerance of 0.02 gives us a spurious failure rate approximately equal to 10^-24.
    ASSERT_GTE(firstTotal / nTrials, 0.73);
    ASSERT_LTE(firstTotal / nTrials, 0.77);

    // The average random meta value of the second document should be about 0.5.
    ASSERT_GTE(secondTotal / nTrials, 0.48);
    ASSERT_LTE(secondTotal / nTrials, 0.52);
}
}  // namespace DocumentSourceSampleFromRandomCursor

}  // namespace DocumentSourceSample

namespace DocumentSourceSort {

using mongo::DocumentSourceSort;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
protected:
    void createSort(const BSONObj& sortKey = BSON("a" << 1)) {
        BSONObj spec = BSON("$sort" << sortKey);
        BSONElement specElement = spec.firstElement();
        _sort = DocumentSourceSort::createFromBson(specElement, ctx());
        checkBsonRepresentation(spec);
    }
    DocumentSourceSort* sort() {
        return dynamic_cast<DocumentSourceSort*>(_sort.get());
    }
    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertExhausted() const {
        ASSERT(!_sort->getNext());
        ASSERT(!_sort->getNext());
        ASSERT(!_sort->getNext());
    }

private:
    /**
     * Check that the BSON representation generated by the souce matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        vector<Value> arr;
        _sort->serializeToArray(arr);
        BSONObj generatedSpec = arr[0].getDocument().toBson();
        ASSERT_EQUALS(spec, generatedSpec);
    }
    intrusive_ptr<DocumentSource> _sort;
};

class SortWithLimit : public Base {
public:
    void run() {
        createSort(BSON("a" << 1));
        ASSERT_EQUALS(sort()->getLimit(), -1);

        Pipeline::SourceContainer container;
        container.push_back(sort());

        {  // pre-limit checks
            vector<Value> arr;
            sort()->serializeToArray(arr);
            ASSERT_EQUALS(arr[0].getDocument().toBson(), BSON("$sort" << BSON("a" << 1)));

            ASSERT(sort()->getShardSource() == NULL);
            ASSERT(sort()->getMergeSource() != NULL);
        }

        container.push_back(mkLimit(10));
        sort()->optimizeAt(container.begin(), &container);
        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(sort()->getLimit(), 10);

        // unchanged
        container.push_back(mkLimit(15));
        sort()->optimizeAt(container.begin(), &container);
        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(sort()->getLimit(), 10);

        // reduced
        container.push_back(mkLimit(5));
        sort()->optimizeAt(container.begin(), &container);
        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(sort()->getLimit(), 5);

        vector<Value> arr;
        sort()->serializeToArray(arr);
        ASSERT_VALUE_EQ(
            Value(arr),
            DOC_ARRAY(DOC("$sort" << DOC("a" << 1)) << DOC("$limit" << sort()->getLimit())));

        ASSERT(sort()->getShardSource() != NULL);
        ASSERT(sort()->getMergeSource() != NULL);
    }

    intrusive_ptr<DocumentSource> mkLimit(int limit) {
        BSONObj obj = BSON("$limit" << limit);
        BSONElement e = obj.firstElement();
        return mongo::DocumentSourceLimit::createFromBson(e, ctx());
    }
};

class CheckResultsBase : public Base {
public:
    virtual ~CheckResultsBase() {}
    void run() {
        createSort(sortSpec());
        auto source = DocumentSourceMock::create(inputData());
        sort()->setSource(source.get());

        // Load the results from the DocumentSourceUnwind.
        vector<Document> resultSet;
        while (boost::optional<Document> current = sort()->getNext()) {
            // Get the current result.
            resultSet.push_back(*current);
        }
        // Verify the DocumentSourceUnwind is exhausted.
        assertExhausted();

        // Convert results to BSON once they all have been retrieved (to detect any errors
        // resulting from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (vector<Document>::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << *i;
        }
        // Check the result set.
        ASSERT_EQUALS(expectedResultSet(), bsonResultSet.arr());
    }

protected:
    virtual std::deque<Document> inputData() {
        return {};
    }
    virtual BSONObj expectedResultSet() {
        BSONObj wrappedResult =
            // fromjson cannot parse an array, so place the array within an object.
            fromjson(string("{'':") + expectedResultSetString() + "}");
        return wrappedResult[""].embeddedObject().getOwned();
    }
    virtual string expectedResultSetString() {
        return "[]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a" << 1);
    }
};

class InvalidSpecBase : public Base {
public:
    virtual ~InvalidSpecBase() {}
    void run() {
        ASSERT_THROWS(createSort(sortSpec()), UserException);
    }

protected:
    virtual BSONObj sortSpec() = 0;
};

class InvalidOperationBase : public Base {
public:
    virtual ~InvalidOperationBase() {}
    void run() {
        createSort(sortSpec());
        auto source = DocumentSourceMock::create(inputData());
        sort()->setSource(source.get());
        ASSERT_THROWS(exhaust(), UserException);
    }

protected:
    virtual std::deque<Document> inputData() = 0;
    virtual BSONObj sortSpec() {
        return BSON("a" << 1);
    }

private:
    void exhaust() {
        while (sort()->getNext()) {
            // do nothing
        }
    }
};

/** No documents in source. */
class Empty : public CheckResultsBase {};

/** Sort a single document. */
class SingleValue : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1)};
    }
    string expectedResultSetString() {
        return "[{_id:0,a:1}]";
    }
};

/** Sort two documents. */
class TwoValues : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 2), DOC("_id" << 1 << "a" << 1)};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:1},{_id:0,a:2}]";
    }
};

/** Sort spec is not an object. */
class NonObjectSpec : public Base {
public:
    void run() {
        BSONObj spec = BSON("$sort" << 1);
        BSONElement specElement = spec.firstElement();
        ASSERT_THROWS(DocumentSourceSort::createFromBson(specElement, ctx()), UserException);
    }
};

/** Sort spec is an empty object. */
class EmptyObjectSpec : public InvalidSpecBase {
    BSONObj sortSpec() {
        return BSONObj();
    }
};

/** Sort spec value is not a number. */
class NonNumberDirectionSpec : public InvalidSpecBase {
    BSONObj sortSpec() {
        return BSON("a"
                    << "b");
    }
};

/** Sort spec value is not a valid number. */
class InvalidNumberDirectionSpec : public InvalidSpecBase {
    BSONObj sortSpec() {
        return BSON("a" << 0);
    }
};

/** Sort spec with a descending field. */
class DescendingOrder : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 2), DOC("_id" << 1 << "a" << 1)};
    }
    string expectedResultSetString() {
        return "[{_id:0,a:2},{_id:1,a:1}]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a" << -1);
    }
};

/** Sort spec with a dotted field. */
class DottedSortField : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC("b" << 2)), DOC("_id" << 1 << "a" << DOC("b" << 1))};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:{b:1}},{_id:0,a:{b:2}}]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a.b" << 1);
    }
};

/** Sort spec with a compound key. */
class CompoundSortSpec : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1 << "b" << 3),
                DOC("_id" << 1 << "a" << 1 << "b" << 2),
                DOC("_id" << 2 << "a" << 0 << "b" << 4)};
    }
    string expectedResultSetString() {
        return "[{_id:2,a:0,b:4},{_id:1,a:1,b:2},{_id:0,a:1,b:3}]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a" << 1 << "b" << 1);
    }
};

/** Sort spec with a compound key and descending order. */
class CompoundSortSpecAlternateOrder : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1 << "b" << 3),
                DOC("_id" << 1 << "a" << 1 << "b" << 2),
                DOC("_id" << 2 << "a" << 0 << "b" << 4)};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:1,b:2},{_id:0,a:1,b:3},{_id:2,a:0,b:4}]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a" << -1 << "b" << 1);
    }
};

/** Sort spec with a compound key and descending order. */
class CompoundSortSpecAlternateOrderSecondField : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1 << "b" << 3),
                DOC("_id" << 1 << "a" << 1 << "b" << 2),
                DOC("_id" << 2 << "a" << 0 << "b" << 4)};
    }
    string expectedResultSetString() {
        return "[{_id:2,a:0,b:4},{_id:0,a:1,b:3},{_id:1,a:1,b:2}]";
    }
    virtual BSONObj sortSpec() {
        return BSON("a" << 1 << "b" << -1);
    }
};

/** Sorting different types is not supported. */
class InconsistentTypeSort : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1),
                DOC("_id" << 1 << "a"
                          << "foo")};
    }
    string expectedResultSetString() {
        return "[{_id:0,a:1},{_id:1,a:\"foo\"}]";
    }
};

/** Sorting different numeric types is supported. */
class MixedNumericSort : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 2.3), DOC("_id" << 1 << "a" << 1)};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:1},{_id:0,a:2.3}]";
    }
};

/** Ordering of a missing value. */
class MissingValue : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1), DOC("_id" << 1)};
    }
    string expectedResultSetString() {
        return "[{_id:1},{_id:0,a:1}]";
    }
};

/** Ordering of a null value. */
class NullValue : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 1), DOC("_id" << 1 << "a" << BSONNULL)};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:null},{_id:0,a:1}]";
    }
};

/**
 * Order by text score.
 */
class TextScore : public CheckResultsBase {
    std::deque<Document> inputData() {
        MutableDocument first;
        first["_id"] = Value(0);
        first.setTextScore(10);
        MutableDocument second;
        second["_id"] = Value(1);
        second.setTextScore(20);
        return {first.freeze(), second.freeze()};
    }

    string expectedResultSetString() {
        return "[{_id:1},{_id:0}]";
    }

    BSONObj sortSpec() {
        return BSON("$computed0" << metaTextScore);
    }
};

/**
 * Order by random value in metadata.
 */
class RandMeta : public CheckResultsBase {
    std::deque<Document> inputData() {
        MutableDocument first;
        first["_id"] = Value(0);
        first.setRandMetaField(0.01);
        MutableDocument second;
        second["_id"] = Value(1);
        second.setRandMetaField(0.02);
        return {first.freeze(), second.freeze()};
    }

    string expectedResultSetString() {
        return "[{_id:1},{_id:0}]";
    }

    BSONObj sortSpec() {
        return BSON("$computed0" << BSON("$meta"
                                         << "randVal"));
    }
};

/** A missing nested object within an array returns an empty array. */
class MissingObjectWithinArray : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1)),
                DOC("_id" << 1 << "a" << DOC_ARRAY(DOC("b" << 1)))};
    }
    string expectedResultSetString() {
        return "[{_id:0,a:[1]},{_id:1,a:[{b:1}]}]";
    }
    BSONObj sortSpec() {
        return BSON("a.b" << 1);
    }
};

/** Compare nested values from within an array. */
class ExtractArrayValues : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(DOC("b" << 1) << DOC("b" << 2))),
                DOC("_id" << 1 << "a" << DOC_ARRAY(DOC("b" << 1) << DOC("b" << 1)))};
    }
    string expectedResultSetString() {
        return "[{_id:1,a:[{b:1},{b:1}]},{_id:0,a:[{b:1},{b:2}]}]";
    }
    BSONObj sortSpec() {
        return BSON("a.b" << 1);
    }
};

/** Dependant field paths. */
class Dependencies : public Base {
public:
    void run() {
        createSort(BSON("a" << 1 << "b.c" << -1));
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, sort()->getDependencies(&dependencies));
        ASSERT_EQUALS(2U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(1U, dependencies.fields.count("b.c"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class OutputSort : public Base {
public:
    void run() {
        createSort(BSON("a" << 1 << "b.c" << -1));
        BSONObjSet outputSort = sort()->getOutputSorts();
        ASSERT_EQUALS(outputSort.count(BSON("a" << 1)), 1U);
        ASSERT_EQUALS(outputSort.count(BSON("a" << 1 << "b.c" << -1)), 1U);
        ASSERT_EQUALS(outputSort.size(), 2U);
    }
};

}  // namespace DocumentSourceSort

namespace DocumentSourceUnwind {

using mongo::DocumentSourceUnwind;
using mongo::DocumentSourceMock;

class CheckResultsBase : public Mock::Base {
public:
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

    virtual std::deque<Document> inputData() {
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
        while (boost::optional<Document> current = _unwind->getNext()) {
            // Get the current result.
            resultSet.push_back(*current);
        }
        // Verify the DocumentSourceUnwind is exhausted.
        assertExhausted();

        // Convert results to BSON once they all have been retrieved (to detect any errors resulting
        // from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (vector<Document>::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << *i;
        }
        // Check the result set.
        ASSERT_EQUALS(expectedResults, bsonResultSet.arr());
    }

    /**
     * Check that the BSON representation generated by the source matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(bool preserveNullAndEmptyArrays, bool includeArrayIndex) {
        vector<Value> arr;
        _unwind->serializeToArray(arr);
        BSONObj generatedSpec = Value(arr[0]).getDocument().toBson();
        ASSERT_EQUALS(expectedSerialization(preserveNullAndEmptyArrays, includeArrayIndex),
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
    void assertExhausted() const {
        ASSERT(!_unwind->getNext());
        ASSERT(!_unwind->getNext());
        ASSERT(!_unwind->getNext());
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

    intrusive_ptr<DocumentSourceUnwind> _unwind;
};

/** An empty collection produces no results. */
class Empty : public CheckResultsBase {};

/**
 * An empty array does not produce any results normally, but if preserveNullAndEmptyArrays is
 * passed, the document is preserved.
 */
class EmptyArray : public CheckResultsBase {
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
        return {DOC("_id" << 0 << "a" << BSONNULL),
                DOC("_id" << 1),
                DOC("_id" << 2 << "a" << DOC_ARRAY("a"
                                                   << "b")),
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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
    std::deque<Document> inputData() override {
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

/** Dependant field paths. */
class Dependencies : public Mock::Base {
public:
    void run() {
        auto unwind =
            DocumentSourceUnwind::create(ctx(), "x.y.z", false, boost::optional<string>("index"));
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, unwind->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("x.y.z"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class OutputSort : public Mock::Base {
public:
    void run() {
        auto unwind = DocumentSourceUnwind::create(ctx(), "x.y", false, boost::none);
        auto source = DocumentSourceMock::create();
        source->sorts = {BSON("a" << 1 << "x.y" << 1 << "b" << 1)};

        unwind->setSource(source.get());

        BSONObjSet outputSort = unwind->getOutputSorts();
        ASSERT_EQUALS(1U, outputSort.size());
        ASSERT_EQUALS(1U, outputSort.count(BSON("a" << 1)));
    }
};

//
// Error cases.
//

/**
 * Fixture to test error cases of the $unwind stage.
 */
class InvalidUnwindSpec : public Mock::Base, public unittest::Test {
public:
    intrusive_ptr<DocumentSource> createUnwind(BSONObj spec) {
        auto specElem = spec.firstElement();
        return DocumentSourceUnwind::createFromBson(specElem, ctx());
    }
};

TEST_F(InvalidUnwindSpec, NonObjectNonString) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << 1)), UserException, 15981);
}

TEST_F(InvalidUnwindSpec, NoPathSpecified) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSONObj())), UserException, 28812);
}

TEST_F(InvalidUnwindSpec, NonStringPath) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path" << 2))), UserException, 28808);
}

TEST_F(InvalidUnwindSpec, NonDollarPrefixedPath) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind"
                                         << "somePath")),
                       UserException,
                       28818);
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "somePath"))),
                       UserException,
                       28818);
}

TEST_F(InvalidUnwindSpec, NonBoolPreserveNullAndEmptyArrays) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "preserveNullAndEmptyArrays"
                                                           << 2))),
                       UserException,
                       28809);
}

TEST_F(InvalidUnwindSpec, NonStringIncludeArrayIndex) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << 2))),
                       UserException,
                       28810);
}

TEST_F(InvalidUnwindSpec, EmptyStringIncludeArrayIndex) {
    ASSERT_THROWS_CODE(createUnwind(BSON("$unwind" << BSON("path"
                                                           << "$x"
                                                           << "includeArrayIndex"
                                                           << ""))),
                       UserException,
                       28810);
}

TEST_F(InvalidUnwindSpec, DollarPrefixedIncludeArrayIndex) {
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

TEST_F(InvalidUnwindSpec, UnrecognizedOption) {
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
}  // namespace DocumentSourceUnwind

namespace DocumentSourceGeoNear {
using mongo::DocumentSourceGeoNear;
using mongo::DocumentSourceLimit;

class LimitCoalesce : public Mock::Base {
public:
    void run() {
        intrusive_ptr<DocumentSourceGeoNear> geoNear = DocumentSourceGeoNear::create(ctx());

        Pipeline::SourceContainer container;
        container.push_back(geoNear);

        ASSERT_EQUALS(geoNear->getLimit(), DocumentSourceGeoNear::kDefaultLimit);

        container.push_back(DocumentSourceLimit::create(ctx(), 200));
        geoNear->optimizeAt(container.begin(), &container);

        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(geoNear->getLimit(), DocumentSourceGeoNear::kDefaultLimit);

        container.push_back(DocumentSourceLimit::create(ctx(), 50));
        geoNear->optimizeAt(container.begin(), &container);

        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(geoNear->getLimit(), 50);

        container.push_back(DocumentSourceLimit::create(ctx(), 30));
        geoNear->optimizeAt(container.begin(), &container);

        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(geoNear->getLimit(), 30);
    }
};

class OutputSort : public Mock::Base {
public:
    void run() {
        BSONObj queryObj = fromjson(
            "{geoNear: { near: {type: 'Point', coordinates: [0, 0]}, distanceField: 'dist', "
            "maxDistance: 2}}");
        intrusive_ptr<DocumentSource> geoNear =
            DocumentSourceGeoNear::createFromBson(queryObj.firstElement(), ctx());

        BSONObjSet outputSort = geoNear->getOutputSorts();

        ASSERT_EQUALS(outputSort.count(BSON("dist" << -1)), 1U);
        ASSERT_EQUALS(outputSort.size(), 1U);
    }
};

}  // namespace DocumentSourceGeoNear

namespace DocumentSourceMatch {
using mongo::DocumentSourceMatch;

using std::unique_ptr;

// Helpers to make a DocumentSourceMatch from a query object or json string
intrusive_ptr<DocumentSourceMatch> makeMatch(const BSONObj& query) {
    intrusive_ptr<DocumentSource> uncasted = DocumentSourceMatch::createFromBson(
        BSON("$match" << query).firstElement(), new ExpressionContext());
    return dynamic_cast<DocumentSourceMatch*>(uncasted.get());
}
intrusive_ptr<DocumentSourceMatch> makeMatch(const string& queryJson) {
    return makeMatch(fromjson(queryJson));
}

class RedactSafePortion {
public:
    void test(string input, string safePortion) {
        try {
            intrusive_ptr<DocumentSourceMatch> match = makeMatch(input);
            ASSERT_EQUALS(match->redactSafePortion(), fromjson(safePortion));
        } catch (...) {
            unittest::log() << "Problem with redactSafePortion() of: " << input;
            throw;
        }
    }

    void run() {
        // Empty
        test("{}", "{}");

        // Basic allowed things
        test("{a:1}", "{a:1}");

        test("{a:'asdf'}", "{a:'asdf'}");

        test("{a:/asdf/i}", "{a:/asdf/i}");

        test("{a: {$regex: 'adsf'}}", "{a: {$regex: 'adsf'}}");

        test("{a: {$regex: 'adsf', $options: 'i'}}", "{a: {$regex: 'adsf', $options: 'i'}}");

        test("{a: {$mod: [1, 0]}}", "{a: {$mod: [1, 0]}}");

        test("{a: {$type: 1}}", "{a: {$type: 1}}");

        // Basic disallowed things
        test("{a: null}", "{}");

        test("{a: {}}", "{}");

        test("{a: []}", "{}");

        test("{'a.0': 1}", "{}");

        test("{'a.0.b': 1}", "{}");

        test("{a: {$ne: 1}}", "{}");

        test("{a: {$nin: [1, 2, 3]}}", "{}");

        test("{a: {$exists: true}}",  // could be allowed but currently isn't
             "{}");

        test("{a: {$exists: false}}",  // can never be allowed
             "{}");

        test("{a: {$size: 1}}", "{}");

        test("{$nor: [{a:1}]}", "{}");

        // Combinations
        test("{a:1, b: 'asdf'}", "{a:1, b: 'asdf'}");

        test("{a:1, b: null}", "{a:1}");

        test("{a:null, b: null}", "{}");

        // $elemMatch

        test("{a: {$elemMatch: {b: 1}}}", "{a: {$elemMatch: {b: 1}}}");

        test("{a: {$elemMatch: {b:null}}}", "{}");

        test("{a: {$elemMatch: {b:null, c:1}}}", "{a: {$elemMatch: {c: 1}}}");

        // explicit $and
        test("{$and:[{a: 1}]}", "{$and:[{a: 1}]}");

        test("{$and:[{a: 1}, {b: null}]}", "{$and:[{a: 1}]}");

        test("{$and:[{a: 1}, {b: null, c:1}]}", "{$and:[{a: 1}, {c:1}]}");

        test("{$and:[{a: null}, {b: null}]}", "{}");

        // explicit $or
        test("{$or:[{a: 1}]}", "{$or:[{a: 1}]}");

        test("{$or:[{a: 1}, {b: null}]}", "{}");

        test("{$or:[{a: 1}, {b: null, c:1}]}", "{$or:[{a: 1}, {c:1}]}");

        test("{$or:[{a: null}, {b: null}]}", "{}");

        test("{}", "{}");

        // $all and $in
        test("{a: {$all: [1, 0]}}", "{a: {$all: [1, 0]}}");

        test("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

        test("{a: {$all: [{$elemMatch: {b:1}}]}}",  // could be allowed but currently isn't
             "{}");

        test("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

        test("{a: {$in: [1, 0]}}", "{a: {$in: [1, 0]}}");

        test("{a: {$in: [1, 0, null]}}", "{}");

        {
            const char* comparisonOps[] = {"$gt", "$lt", "$gte", "$lte", NULL};
            for (int i = 0; comparisonOps[i]; i++) {
                const char* op = comparisonOps[i];
                test(string("{a: {") + op + ": 1}}", string("{a: {") + op + ": 1}}");

                // $elemMatch takes direct expressions ...
                test(string("{a: {$elemMatch: {") + op + ": 1}}}",
                     string("{a: {$elemMatch: {") + op + ": 1}}}");

                // ... or top-level style full matches
                test(string("{a: {$elemMatch: {b: {") + op + ": 1}}}}",
                     string("{a: {$elemMatch: {b: {") + op + ": 1}}}}");

                test(string("{a: {") + op + ": null}}", "{}");

                test(string("{a: {") + op + ": {}}}", "{}");

                test(string("{a: {") + op + ": []}}", "{}");

                test(string("{'a.0': {") + op + ": null}}", "{}");

                test(string("{'a.0.b': {") + op + ": null}}", "{}");
            }
        }
    }
};

class DependenciesOrExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{$or: [{a: 1}, {'x.y': {$gt: 4}}]}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(1U, dependencies.fields.count("x.y"));
        ASSERT_EQUALS(2U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesTextExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{$text: {$search: 'hello'} }");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, match->getDependencies(&dependencies));
        ASSERT_EQUALS(true, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesGTEExpression {
public:
    void run() {
        // Parses to {a: {$eq: {notAField: {$gte: 4}}}}.
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{a: {notAField: {$gte: 4}}}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesElemMatchExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{a: {$elemMatch: {c: {$gte: 4}}}}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a.c"));
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(2U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesElemMatchWithNoSubfield {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{a: {$elemMatch: {$gt: 1, $lt: 5}}}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};
class DependenciesNotExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{b: {$not: {$gte: 4}}}}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("b"));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesNorExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match =
            makeMatch("{$nor: [{'a.b': {$gte: 4}}, {'b.c': {$in: [1, 2]}}]}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(1U, dependencies.fields.count("b.c"));
        ASSERT_EQUALS(2U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesCommentExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{$comment: 'misleading?'}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(0U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class DependenciesCommentMatchExpression {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match = makeMatch("{a: 4, $comment: 'irrelevant'}");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.count("a"));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

class Coalesce {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match1 = makeMatch(BSON("a" << 1));
        intrusive_ptr<DocumentSourceMatch> match2 = makeMatch(BSON("b" << 1));
        intrusive_ptr<DocumentSourceMatch> match3 = makeMatch(BSON("c" << 1));

        Pipeline::SourceContainer container;

        // Check initial state
        ASSERT_EQUALS(match1->getQuery(), BSON("a" << 1));
        ASSERT_EQUALS(match2->getQuery(), BSON("b" << 1));
        ASSERT_EQUALS(match3->getQuery(), BSON("c" << 1));

        container.push_back(match1);
        container.push_back(match2);
        match1->optimizeAt(container.begin(), &container);

        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(match1->getQuery(), fromjson("{'$and': [{a:1}, {b:1}]}"));

        container.push_back(match3);
        match1->optimizeAt(container.begin(), &container);
        ASSERT_EQUALS(container.size(), 1U);
        ASSERT_EQUALS(match1->getQuery(),
                      fromjson("{'$and': [{'$and': [{a:1}, {b:1}]},"
                               "{c:1}]}"));
    }
};

TEST(ObjectForMatch, ShouldExtractTopLevelFieldIfDottedFieldNeeded) {
    Document input(fromjson("{a: 1, b: {c: 1, d: 1}}"));
    BSONObj expected = fromjson("{b: {c: 1, d: 1}}");
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, {"b.c"}));
}

TEST(ObjectForMatch, ShouldExtractEntireArray) {
    Document input(fromjson("{a: [1, 2, 3], b: 1}"));
    BSONObj expected = fromjson("{a: [1, 2, 3]}");
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, {"a"}));
}

TEST(ObjectForMatch, ShouldOnlyAddPrefixedFieldOnceIfTwoDottedSubfields) {
    Document input(fromjson("{a: 1, b: {c: 1, f: {d: {e: 1}}}}"));
    BSONObj expected = fromjson("{b: {c: 1, f: {d: {e: 1}}}}");
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, {"b.f", "b.f.d.e"}));
}

TEST(ObjectForMatch, MissingFieldShouldNotAppearInResult) {
    Document input(fromjson("{a: 1}"));
    BSONObj expected;
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, {"b", "c"}));
}

TEST(ObjectForMatch, ShouldSerializeNothingIfNothingIsNeeded) {
    Document input(fromjson("{a: 1, b: {c: 1}}"));
    BSONObj expected;
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, std::set<std::string>{}));
}

TEST(ObjectForMatch, ShouldExtractEntireArrayFromPrefixOfDottedField) {
    Document input(fromjson("{a: [{b: 1}, {b: 2}], c: 1}"));
    BSONObj expected = fromjson("{a: [{b: 1}, {b: 2}]}");
    ASSERT_EQUALS(expected, DocumentSourceMatch::getObjectForMatch(input, {"a.b"}));
}


}  // namespace DocumentSourceMatch

namespace DocumentSourceLookUp {
using mongo::DocumentSourceLookUp;

class OutputSortTruncatesOnEquality : public Mock::Base {
public:
    void run() {
        intrusive_ptr<DocumentSourceMock> source = DocumentSourceMock::create();
        source->sorts = {BSON("a" << 1 << "d.e" << 1 << "c" << 1)};
        intrusive_ptr<DocumentSource> lookup =
            DocumentSourceLookUp::createFromBson(BSON("$lookup" << BSON("from"
                                                                        << "a"
                                                                        << "localField"
                                                                        << "b"
                                                                        << "foreignField"
                                                                        << "c"
                                                                        << "as"
                                                                        << "d.e"))
                                                     .firstElement(),
                                                 ctx());
        lookup->setSource(source.get());

        BSONObjSet outputSort = lookup->getOutputSorts();

        ASSERT_EQUALS(outputSort.count(BSON("a" << 1)), 1U);
        ASSERT_EQUALS(outputSort.size(), 1U);
    }
};

class OutputSortTruncatesOnPrefix : public Mock::Base {
public:
    void run() {
        intrusive_ptr<DocumentSourceMock> source = DocumentSourceMock::create();
        source->sorts = {BSON("a" << 1 << "d.e" << 1 << "c" << 1)};
        intrusive_ptr<DocumentSource> lookup =
            DocumentSourceLookUp::createFromBson(BSON("$lookup" << BSON("from"
                                                                        << "a"
                                                                        << "localField"
                                                                        << "b"
                                                                        << "foreignField"
                                                                        << "c"
                                                                        << "as"
                                                                        << "d"))
                                                     .firstElement(),
                                                 ctx());
        lookup->setSource(source.get());

        BSONObjSet outputSort = lookup->getOutputSorts();

        ASSERT_EQUALS(outputSort.count(BSON("a" << 1)), 1U);
        ASSERT_EQUALS(outputSort.size(), 1U);
    }
};
}

namespace DocumentSourceSortByCount {
using mongo::DocumentSourceSortByCount;
using mongo::DocumentSourceGroup;
using mongo::DocumentSourceSort;
using std::vector;
using boost::intrusive_ptr;

/**
 * Fixture to test that $sortByCount returns a DocumentSourceGroup and DocumentSourceSort.
 */
class SortByCountReturnsGroupAndSort : public Mock::Base, public unittest::Test {
public:
    void testCreateFromBsonResult(BSONObj sortByCountSpec, Value expectedGroupExplain) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceSortByCount::createFromBson(sortByCountSpec.firstElement(), ctx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result[1].get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $sortByCount so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        const bool explain = true;
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        sortStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        auto sortExplain = explainedStages[1];
        auto expectedSortExplain = Value{Document{{"sortKey", Document{{"count", -1}}}}};
        ASSERT_VALUE_EQ(sortExplain["$sort"], expectedSortExplain);
    }
};

TEST_F(SortByCountReturnsGroupAndSort, ExpressionFieldPathSpec) {
    BSONObj spec = BSON("$sortByCount"
                        << "$x");
    Value expectedGroupExplain =
        Value{Document{{"_id", "$x"}, {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(SortByCountReturnsGroupAndSort, ExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("$floor"
                                               << "$x"));
    Value expectedGroupExplain =
        Value{Document{{"_id", Document{{"$floor", Value{BSON_ARRAY("$x")}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);

    spec = BSON("$sortByCount" << BSON("$eq" << BSON_ARRAY("$x" << 15)));
    expectedGroupExplain =
        Value{Document{{"_id", Document{{"$eq", Value{BSON_ARRAY("$x" << BSON("$const" << 15))}}}},
                       {"count", Document{{"$sum", Document{{"$const", 1}}}}}}};
    testCreateFromBsonResult(spec, expectedGroupExplain);
}

/**
 * Fixture to test error cases of the $sortByCount stage.
 */
class InvalidSortByCountSpec : public Mock::Base, public unittest::Test {
public:
    vector<intrusive_ptr<DocumentSource>> createSortByCount(BSONObj sortByCountSpec) {
        auto specElem = sortByCountSpec.firstElement();
        return DocumentSourceSortByCount::createFromBson(specElem, ctx());
    }
};

TEST_F(InvalidSortByCountSpec, NonObjectNonStringSpec) {
    BSONObj spec = BSON("$sortByCount" << 1);
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40149);

    spec = BSON("$sortByCount" << BSONNULL);
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40149);
}

TEST_F(InvalidSortByCountSpec, NonExpressionInObjectSpec) {
    BSONObj spec = BSON("$sortByCount" << BSON("field1"
                                               << "$x"));
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40147);
}

TEST_F(InvalidSortByCountSpec, NonFieldPathStringSpec) {
    BSONObj spec = BSON("$sortByCount"
                        << "test");
    ASSERT_THROWS_CODE(createSortByCount(spec), UserException, 40148);
}
}  // namespace DocumentSourceSortByCount

namespace DocumentSourceCount {
using mongo::DocumentSourceCount;
using mongo::DocumentSourceGroup;
using mongo::DocumentSourceProject;
using std::vector;
using boost::intrusive_ptr;

class CountReturnsGroupAndProjectStages : public Mock::Base, public unittest::Test {
public:
    void testCreateFromBsonResult(BSONObj countSpec) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceCount::createFromBson(countSpec.firstElement(), ctx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* projectStage = dynamic_cast<DocumentSourceProject*>(result[1].get());
        ASSERT(projectStage);

        const bool explain = true;
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        projectStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        StringData countName = countSpec.firstElement().valueStringData();
        Value expectedGroupExplain =
            Value{Document{{"_id", Document{{"$const", BSONNULL}}},
                           {countName, Document{{"$sum", Document{{"$const", 1}}}}}}};
        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        Value expectedProjectExplain = Value{Document{{"_id", false}, {countName, true}}};
        auto projectExplain = explainedStages[1];
        ASSERT_VALUE_EQ(projectExplain["$project"], expectedProjectExplain);
    }
};

TEST_F(CountReturnsGroupAndProjectStages, ValidStringSpec) {
    BSONObj spec = BSON("$count"
                        << "myCount");
    testCreateFromBsonResult(spec);

    spec = BSON("$count"
                << "quantity");
    testCreateFromBsonResult(spec);
}

class InvalidCountSpec : public Mock::Base, public unittest::Test {
public:
    vector<intrusive_ptr<DocumentSource>> createCount(BSONObj countSpec) {
        auto specElem = countSpec.firstElement();
        return DocumentSourceCount::createFromBson(specElem, ctx());
    }
};

TEST_F(InvalidCountSpec, NonStringSpec) {
    BSONObj spec = BSON("$count" << 1);
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40156);

    spec = BSON("$count" << BSON("field1"
                                 << "test"));
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40156);
}

TEST_F(InvalidCountSpec, EmptyStringSpec) {
    BSONObj spec = BSON("$count"
                        << "");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40157);
}

TEST_F(InvalidCountSpec, FieldPathSpec) {
    BSONObj spec = BSON("$count"
                        << "$x");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40158);
}

TEST_F(InvalidCountSpec, EmbeddedNullByteSpec) {
    BSONObj spec = BSON("$count"
                        << "te\0st"_sd);
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40159);
}

TEST_F(InvalidCountSpec, PeriodInStringSpec) {
    BSONObj spec = BSON("$count"
                        << "test.string");
    ASSERT_THROWS_CODE(createCount(spec), UserException, 40160);
}
}  // namespace DocumentSourceCount

namespace DocumentSourceBucket {
using mongo::DocumentSourceBucket;
using mongo::DocumentSourceGroup;
using mongo::DocumentSourceSort;
using mongo::DocumentSourceMock;
using std::vector;
using boost::intrusive_ptr;

class BucketReturnsGroupAndSort : public Mock::Base, public unittest::Test {
public:
    void testCreateFromBsonResult(BSONObj bucketSpec, Value expectedGroupExplain) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), ctx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result[1].get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $bucket so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        const bool explain = true;
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        sortStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        auto sortExplain = explainedStages[1];

        auto expectedSortExplain = Value{Document{{"sortKey", Document{{"_id", 1}}}}};
        ASSERT_VALUE_EQ(sortExplain["$sort"], expectedSortExplain);
    }
};

TEST_F(BucketReturnsGroupAndSort, BucketUsesDefaultOutputWhenNoOutputSpecified) {
    const auto spec =
        fromjson("{$bucket : {groupBy :'$x', boundaries : [ 0, 2 ], default : 'other'}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                       "0}]}, {$lt : ['$x', {$const : 2}]}]}, then : {$const : 0}}], default : "
                       "{$const : 'other'}}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenOutputSpecified) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [0, 2], output : { number : {$sum : 1}}}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, number : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : { groupBy : '$x', boundaries : [0, 2]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreSameCanonicalType) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1.5]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 1.5}]}]}, then : {$const : 0}}]}},count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreConstantExpressions) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, {$add : [4, 5]}]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 9}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenDefaultIsConstantExpression) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1], default: {$add : [4, 5]}}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const :"
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}], default : "
                       "{$const : 9}}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWithMultipleBoundaryValues) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2]}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}, {case : {$and "
                       ": [{$gte : ['$x', {$const : 1}]}, {$lt : ['$x', {$const : 2}]}]}, then : "
                       "{$const : 1}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

class InvalidBucketSpec : public Mock::Base, public unittest::Test {
public:
    vector<intrusive_ptr<DocumentSource>> createBucket(BSONObj bucketSpec) {
        return DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), ctx());
    }
};

TEST_F(InvalidBucketSpec, BucketFailsWithNonObject) {
    auto spec = fromjson("{$bucket : 1}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40201);

    spec = fromjson("{$bucket : 'test'}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40201);
}

TEST_F(InvalidBucketSpec, BucketFailsWithUnknownField) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], unknown : 'field'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40197);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoGroupBy) {
    const auto spec = fromjson("{$bucket : {boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonExpressionGroupBy) {
    auto spec = fromjson("{$bucket : {groupBy : {test : 'obj'}, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);

    spec = fromjson("{$bucket : {groupBy : 'test', boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);

    spec = fromjson("{$bucket : {groupBy : 1, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonArrayBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : {test : 'obj'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNotEnoughBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40192);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : []}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40192);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantValueBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['$x', '$y', '$z']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40191);
}

TEST_F(InvalidBucketSpec, BucketFailsWithMixedTypesBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 'test']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40193);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonUniqueBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 1, 2, 3]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['a', 'b', 'b', 'c']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonSortedBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [4, 5, 3, 6]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantExpressionDefault) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], default : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40195);
}

TEST_F(InvalidBucketSpec, BucketFailsWhenDefaultIsInBoundariesRange) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 3}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40199);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40199);
}

TEST_F(InvalidBucketSpec, GroupFailsForBucketWithInvalidOutputField) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40196);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {number : 'test'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 15951);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {'test.test' : {$sum : "
        "1}}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 16414);
}

TEST_F(InvalidBucketSpec, SwitchFailsForBucketWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3]}}");
    vector<intrusive_ptr<DocumentSource>> bucketStages = createBucket(spec);

    ASSERT_EQUALS(bucketStages.size(), 2UL);

    auto* groupStage = dynamic_cast<DocumentSourceGroup*>(bucketStages[0].get());
    ASSERT(groupStage);

    const auto* sortStage = dynamic_cast<DocumentSourceSort*>(bucketStages[1].get());
    ASSERT(sortStage);

    auto doc = DOC("x" << 4);
    auto source = DocumentSourceMock::create(doc);
    groupStage->setSource(source.get());
    ASSERT_THROWS_CODE(groupStage->getNext(), UserException, 40066);
}
}  // namespace DocumentSourceBucket

class All : public Suite {
public:
    All() : Suite("documentsource") {}
    void setupTests() {
        add<DocumentSourceClass::Deps>();

        add<DocumentSourceLimit::DisposeSource>();
        add<DocumentSourceLimit::CombineLimit>();
        add<DocumentSourceLimit::DisposeSourceCascade>();
        add<DocumentSourceLimit::Dependencies>();

        add<DocumentSourceGroup::NonObject>();
        add<DocumentSourceGroup::EmptySpec>();
        add<DocumentSourceGroup::IdEmptyObject>();
        add<DocumentSourceGroup::IdObjectExpression>();
        add<DocumentSourceGroup::IdInvalidObjectExpression>();
        add<DocumentSourceGroup::TwoIdSpecs>();
        add<DocumentSourceGroup::IdEmptyString>();
        add<DocumentSourceGroup::IdStringConstant>();
        add<DocumentSourceGroup::IdFieldPath>();
        add<DocumentSourceGroup::IdInvalidFieldPath>();
        add<DocumentSourceGroup::IdNumericConstant>();
        add<DocumentSourceGroup::IdArrayConstant>();
        add<DocumentSourceGroup::IdRegularExpression>();
        add<DocumentSourceGroup::DollarAggregateFieldName>();
        add<DocumentSourceGroup::NonObjectAggregateSpec>();
        add<DocumentSourceGroup::EmptyObjectAggregateSpec>();
        add<DocumentSourceGroup::BadAccumulator>();
        add<DocumentSourceGroup::SumArray>();
        add<DocumentSourceGroup::MultipleAccumulatorsForAField>();
        add<DocumentSourceGroup::DuplicateAggregateFieldNames>();
        add<DocumentSourceGroup::AggregateObjectExpression>();
        add<DocumentSourceGroup::AggregateOperatorExpression>();
        add<DocumentSourceGroup::EmptyCollection>();
        add<DocumentSourceGroup::SingleDocument>();
        add<DocumentSourceGroup::TwoValuesSingleKey>();
        add<DocumentSourceGroup::TwoValuesTwoKeys>();
        add<DocumentSourceGroup::FourValuesTwoKeys>();
        add<DocumentSourceGroup::FourValuesTwoKeysTwoAccumulators>();
        add<DocumentSourceGroup::GroupNullUndefinedIds>();
        add<DocumentSourceGroup::ComplexId>();
        add<DocumentSourceGroup::UndefinedAccumulatorValue>();
        add<DocumentSourceGroup::RouterMerger>();
        add<DocumentSourceGroup::Dependencies>();
        add<DocumentSourceGroup::StringConstantIdAndAccumulatorExpressions>();
        add<DocumentSourceGroup::ArrayConstantAccumulatorExpression>();
#if 0
        // Disabled tests until SERVER-23318 is implemented.
        add<DocumentSourceGroup::StreamingOptimization>();
        add<DocumentSourceGroup::StreamingWithMultipleIdFields>();
        add<DocumentSourceGroup::NoOptimizationIfMissingDoubleSort>();
        add<DocumentSourceGroup::NoOptimizationWithRawRoot>();
        add<DocumentSourceGroup::NoOptimizationIfUsingExpressions>();
        add<DocumentSourceGroup::StreamingWithMultipleLevels>();
        add<DocumentSourceGroup::StreamingWithConstant>();
        add<DocumentSourceGroup::StreamingWithEmptyId>();
        add<DocumentSourceGroup::StreamingWithRootSubfield>();
        add<DocumentSourceGroup::StreamingWithConstantAndFieldPath>();
        add<DocumentSourceGroup::StreamingWithFieldRepeated>();
#endif

        add<DocumentSourceSort::Empty>();
        add<DocumentSourceSort::SingleValue>();
        add<DocumentSourceSort::TwoValues>();
        add<DocumentSourceSort::NonObjectSpec>();
        add<DocumentSourceSort::EmptyObjectSpec>();
        add<DocumentSourceSort::NonNumberDirectionSpec>();
        add<DocumentSourceSort::InvalidNumberDirectionSpec>();
        add<DocumentSourceSort::DescendingOrder>();
        add<DocumentSourceSort::DottedSortField>();
        add<DocumentSourceSort::CompoundSortSpec>();
        add<DocumentSourceSort::CompoundSortSpecAlternateOrder>();
        add<DocumentSourceSort::CompoundSortSpecAlternateOrderSecondField>();
        add<DocumentSourceSort::InconsistentTypeSort>();
        add<DocumentSourceSort::MixedNumericSort>();
        add<DocumentSourceSort::MissingValue>();
        add<DocumentSourceSort::NullValue>();
        add<DocumentSourceSort::TextScore>();
        add<DocumentSourceSort::RandMeta>();
        add<DocumentSourceSort::MissingObjectWithinArray>();
        add<DocumentSourceSort::ExtractArrayValues>();
        add<DocumentSourceSort::Dependencies>();
        add<DocumentSourceSort::OutputSort>();

        add<DocumentSourceUnwind::Empty>();
        add<DocumentSourceUnwind::EmptyArray>();
        add<DocumentSourceUnwind::MissingValue>();
        add<DocumentSourceUnwind::Null>();
        add<DocumentSourceUnwind::Undefined>();
        add<DocumentSourceUnwind::OneValue>();
        add<DocumentSourceUnwind::TwoValues>();
        add<DocumentSourceUnwind::ArrayWithNull>();
        add<DocumentSourceUnwind::TwoDocuments>();
        add<DocumentSourceUnwind::NestedArray>();
        add<DocumentSourceUnwind::NonObjectParent>();
        add<DocumentSourceUnwind::DoubleNestedArray>();
        add<DocumentSourceUnwind::SeveralDocuments>();
        add<DocumentSourceUnwind::SeveralMoreDocuments>();
        add<DocumentSourceUnwind::Dependencies>();
        add<DocumentSourceUnwind::OutputSort>();
        add<DocumentSourceUnwind::IncludeArrayIndexSubObject>();
        add<DocumentSourceUnwind::IncludeArrayIndexOverrideExisting>();
        add<DocumentSourceUnwind::IncludeArrayIndexOverrideExistingNested>();
        add<DocumentSourceUnwind::IncludeArrayIndexOverrideUnwindPath>();
        add<DocumentSourceUnwind::IncludeArrayIndexWithinUnwindPath>();

        add<DocumentSourceGeoNear::LimitCoalesce>();
        add<DocumentSourceGeoNear::OutputSort>();

        add<DocumentSourceLookUp::OutputSortTruncatesOnEquality>();
        add<DocumentSourceLookUp::OutputSortTruncatesOnPrefix>();

        add<DocumentSourceMatch::RedactSafePortion>();
        add<DocumentSourceMatch::Coalesce>();
        add<DocumentSourceMatch::DependenciesOrExpression>();
        add<DocumentSourceMatch::DependenciesGTEExpression>();
        add<DocumentSourceMatch::DependenciesElemMatchExpression>();
        add<DocumentSourceMatch::DependenciesElemMatchWithNoSubfield>();
        add<DocumentSourceMatch::DependenciesNotExpression>();
        add<DocumentSourceMatch::DependenciesNorExpression>();
        add<DocumentSourceMatch::DependenciesCommentExpression>();
        add<DocumentSourceMatch::DependenciesCommentMatchExpression>();
    }
};

SuiteInstance<All> myall;

}  // namespace DocumentSourceTests
