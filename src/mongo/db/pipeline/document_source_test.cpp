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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {
bool isMongos() {
    return false;
}
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

template <size_t ArrayLen>
set<string> arrayToSet(const char*(&array)[ArrayLen]) {
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
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            deps.needWholeDocument = true;
            deps.needTextScore = true;
            ASSERT_EQUALS(deps.toProjection(), BSON(Document::metaFieldTextScore << metaTextScore));
        }
        {
            const char* array[] = {"a"};  // needTextScore without needWholeDocument
            DepsTracker deps;
            deps.fields = arrayToSet(array);
            deps.needTextScore = true;
            ASSERT_EQUALS(
                deps.toProjection(),
                BSON(Document::metaFieldTextScore << metaTextScore << "a" << 1 << "_id" << 0));
        }
    }
};
}

namespace Mock {
using mongo::DocumentSourceMock;

class Base {
public:
    Base()
        : _service(),
          _client(_service.makeClient("DocumentSourceTest")),
          _opCtx(_client->makeOperationContext()),
          _ctx(new ExpressionContext(_opCtx.get(), NamespaceString(ns))) {}

protected:
    intrusive_ptr<ExpressionContext> ctx() {
        return _ctx;
    }

    ServiceContextNoop _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    intrusive_ptr<ExpressionContext> _ctx;
};

TEST(Mock, OneDoc) {
    auto doc = DOC("a" << 1);
    auto source = DocumentSourceMock::create(doc);
    ASSERT_EQ(*source->getNext(), doc);
    ASSERT(!source->getNext());
}

TEST(Mock, DequeDocuments) {
    auto source = DocumentSourceMock::create({DOC("a" << 1), DOC("a" << 2)});
    ASSERT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT_EQ(*source->getNext(), DOC("a" << 2));
    ASSERT(!source->getNext());
}

TEST(Mock, StringJSON) {
    auto source = DocumentSourceMock::create("{a : 1}");
    ASSERT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT(!source->getNext());
}

TEST(Mock, DequeStringJSONs) {
    auto source = DocumentSourceMock::create({"{a: 1}", "{a: 2}"});
    ASSERT_EQ(*source->getNext(), DOC("a" << 1));
    ASSERT_EQ(*source->getNext(), DOC("a" << 2));
    ASSERT(!source->getNext());
}

TEST(Mock, Empty) {
    auto source = DocumentSourceMock::create();
    ASSERT(!source->getNext());
}

}  // namespace Mock

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
        ASSERT_EQUALS(Value(1), next->getField("a"));
        // The limit is exhausted.
        ASSERT(!limit()->getNext());
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
        ASSERT_EQUALS(Value(1), next->getField("a"));
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
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

}  // namespace DocumentSourceLimit

namespace DocumentSourceGroup {

using mongo::DocumentSourceGroup;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
public:
    Base() : _tempDir("DocumentSourceGroupTest") {}

protected:
    void createGroup(const BSONObj& spec, bool inShard = false) {
        BSONObj namedSpec = BSON("$group" << spec);
        BSONElement specElement = namedSpec.firstElement();

        intrusive_ptr<ExpressionContext> expressionContext =
            new ExpressionContext(_opCtx.get(), NamespaceString(ns));
        expressionContext->inShard = inShard;
        // Won't spill to disk properly if it needs to.
        expressionContext->tempDir = _tempDir.path();

        _group = DocumentSourceGroup::createFromBson(specElement, expressionContext);
        assertRoundTrips(_group);
    }
    DocumentSource* group() {
        return _group.get();
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
                    << "a" << BSON("$push"
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
                    << "a" << BSON("$push"
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
                    << "list" << BSON("$push"
                                      << "$a") << "sum"
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
                    << "sum" << BSON("$sum"
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
                         << "list" << BSON("$push"
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
        ASSERT_EQUALS(false, dependencies.needTextScore);
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

using mongo::DocumentSourceProject;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
protected:
    void createProject(const BSONObj& projection = BSON("a" << true)) {
        BSONObj spec = BSON("$project" << projection);
        BSONElement specElement = spec.firstElement();
        _project = DocumentSourceProject::createFromBson(specElement, ctx());
        checkBsonRepresentation(spec);
    }
    DocumentSource* project() {
        return _project.get();
    }
    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertExhausted() const {
        ASSERT(!_project->getNext());
        ASSERT(!_project->getNext());
        ASSERT(!_project->getNext());
    }
    /**
     * Check that the BSON representation generated by the souce matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        vector<Value> arr;
        _project->serializeToArray(arr);
        BSONObj generatedSpec = arr[0].getDocument().toBson();
        ASSERT_EQUALS(spec, generatedSpec);
    }

private:
    intrusive_ptr<DocumentSource> _project;
};

/** The 'a' and 'c.d' fields are included, but the 'b' field is not. */
class Inclusion : public Base {
public:
    void run() {
        createProject(BSON("a" << true << "c" << BSON("d" << true)));
        auto source = DocumentSourceMock::create("{_id:0,a:1,b:1,c:{d:1}}");
        project()->setSource(source.get());
        // The first result exists and is as expected.
        boost::optional<Document> next = project()->getNext();
        ASSERT(bool(next));
        ASSERT_EQUALS(1, next->getField("a").getInt());
        ASSERT(next->getField("b").missing());
        // The _id field is included by default in the root document.
        ASSERT_EQUALS(0, next->getField("_id").getInt());
        // The nested c.d inclusion.
        ASSERT_EQUALS(1, (*next)["c"]["d"].getInt());
    }
};

/** Optimize the projection. */
class Optimize : public Base {
public:
    void run() {
        createProject(BSON("a" << BSON("$and" << BSON_ARRAY(BSON("$const" << true)))));
        project()->optimize();
        // Optimizing the DocumentSourceProject optimizes the Expressions that comprise it,
        // in this case replacing an expression depending on constants with a constant.
        checkBsonRepresentation(fromjson("{$project:{a:{$const:true}}}"));
    }
};

/** Projection spec is not an object. */
class NonObjectSpec : public Base {
public:
    void run() {
        BSONObj spec = BSON("$project"
                            << "foo");
        BSONElement specElement = spec.firstElement();
        ASSERT_THROWS(DocumentSourceProject::createFromBson(specElement, ctx()), UserException);
    }
};

/** Projection spec is an empty object. */
class EmptyObjectSpec : public Base {
public:
    void run() {
        ASSERT_THROWS(createProject(BSONObj()), UserException);
    }
};

/** Projection spec contains a top level dollar sign. */
class TopLevelDollar : public Base {
public:
    void run() {
        ASSERT_THROWS(createProject(BSON("$add" << BSONArray())), UserException);
    }
};

/** Projection spec is invalid. */
class InvalidSpec : public Base {
public:
    void run() {
        ASSERT_THROWS(createProject(BSON("a" << BSON("$invalidOperator" << 1))), UserException);
    }
};

/** Two documents are projected. */
class TwoDocuments : public Base {
public:
    void run() {
        createProject();
        auto source = DocumentSourceMock::create({"{a: 1, b:2}", "{a: 3, b: 4}"});
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
    }
};

/** List of dependent field paths. */
class Dependencies : public Base {
public:
    void run() {
        createProject(fromjson("{a:true,x:'$b',y:{$and:['$c','$d']}, z: {$meta:'textScore'}}"));
        DepsTracker dependencies;
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
        ASSERT_EQUALS(true, dependencies.needTextScore);
    }
};

}  // namespace DocumentSourceProject

namespace DocumentSourceSample {

using mongo::DocumentSourceSample;
using mongo::DocumentSourceMock;

class SampleBasics : public Mock::Base, public unittest::Test {
public:
    SampleBasics() : _mock(DocumentSourceMock::create()) {}

protected:
    void createSample(long long size) {
        BSONObj spec = BSON("$sample" << BSON("size" << size));
        BSONElement specElement = spec.firstElement();
        _sample = DocumentSourceSample::createFromBson(specElement, ctx());
        sample()->setSource(_mock.get());
        checkBsonRepresentation(spec);
    }

    DocumentSourceSample* sample() {
        return static_cast<DocumentSourceSample*>(_sample.get());
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

private:
    /**
     * Check that the BSON representation generated by the souce matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        Value serialized = sample()->serialize(false);
        auto generatedSpec = serialized.getDocument().toBson();
        ASSERT_EQUALS(spec, generatedSpec);
    }

    intrusive_ptr<DocumentSource> _sample;
    intrusive_ptr<DocumentSourceMock> _mock;
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
    ASSERT_THROWS_CODE(createSample(BSON("$sample" << 1)), UserException, 28739);
    ASSERT_THROWS_CODE(createSample(BSON("$sample"
                                         << "string")),
                       UserException,
                       28739);
}

TEST_F(InvalidSampleSpec, NonNumericSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size"
                                                    << "string"))),
                       UserException,
                       28740);
}

TEST_F(InvalidSampleSpec, NegativeSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1))), UserException, 28741);
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1.0))), UserException, 28741);
}

TEST_F(InvalidSampleSpec, ExtraOption) {
    ASSERT_THROWS_CODE(
        createSample(createSpec(BSON("size" << 1 << "extra" << 2))), UserException, 28742);
}

TEST_F(InvalidSampleSpec, MissingSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSONObj())), UserException, 28743);
}

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

        {  // pre-limit checks
            vector<Value> arr;
            sort()->serializeToArray(arr);
            ASSERT_EQUALS(arr[0].getDocument().toBson(), BSON("$sort" << BSON("a" << 1)));

            ASSERT(sort()->getShardSource() == NULL);
            ASSERT(sort()->getMergeSource() != NULL);
        }

        ASSERT_TRUE(sort()->coalesce(mkLimit(10)));
        ASSERT_EQUALS(sort()->getLimit(), 10);
        ASSERT_TRUE(sort()->coalesce(mkLimit(15)));
        ASSERT_EQUALS(sort()->getLimit(), 10);  // unchanged
        ASSERT_TRUE(sort()->coalesce(mkLimit(5)));
        ASSERT_EQUALS(sort()->getLimit(), 5);  // reduced

        vector<Value> arr;
        sort()->serializeToArray(arr);
        ASSERT_EQUALS(
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
        first.setRandMetaField(10);
        MutableDocument second;
        second["_id"] = Value(1);
        second.setRandMetaField(20);
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
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

}  // namespace DocumentSourceSort

namespace DocumentSourceUnwind {

using mongo::DocumentSourceUnwind;
using mongo::DocumentSourceMock;

class Base : public Mock::Base {
protected:
    void createUnwind(const string& unwindFieldPath = "$a") {
        BSONObj spec = BSON("$unwind" << unwindFieldPath);
        BSONElement specElement = spec.firstElement();
        _unwind = DocumentSourceUnwind::createFromBson(specElement, ctx());
        checkBsonRepresentation(spec);
    }
    DocumentSource* unwind() {
        return _unwind.get();
    }
    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertExhausted() const {
        ASSERT(!_unwind->getNext());
        ASSERT(!_unwind->getNext());
        ASSERT(!_unwind->getNext());
    }

private:
    /**
     * Check that the BSON representation generated by the source matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        vector<Value> arr;
        _unwind->serializeToArray(arr);
        BSONObj generatedSpec = Value(arr[0]).getDocument().toBson();
        ASSERT_EQUALS(spec, generatedSpec);
    }
    intrusive_ptr<DocumentSource> _unwind;
};

class CheckResultsBase : public Base {
public:
    virtual ~CheckResultsBase() {}
    void run() {
        createUnwind(unwindFieldPath());
        auto source = DocumentSourceMock::create(inputData());
        unwind()->setSource(source.get());
        // Load the results from the DocumentSourceUnwind.
        vector<Document> resultSet;
        while (boost::optional<Document> current = unwind()->getNext()) {
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
    virtual BSONObj expectedResultSet() const {
        BSONObj wrappedResult =
            // fromjson cannot parse an array, so place the array within an object.
            fromjson(string("{'':") + expectedResultSetString() + "}");
        return wrappedResult[""].embeddedObject().getOwned();
    }
    virtual string expectedResultSetString() const {
        return "[]";
    }
    virtual string unwindFieldPath() const {
        return "$a";
    }
};

class UnexpectedTypeBase : public Base {
public:
    virtual ~UnexpectedTypeBase() {}
    void run() {
        createUnwind();
        auto source = DocumentSourceMock::create(inputData());
        unwind()->setSource(source.get());
        // A UserException is thrown during iteration.
        ASSERT_THROWS(iterateAll(), UserException);
    }

protected:
    virtual std::deque<Document> inputData() {
        return {};
    }

private:
    void iterateAll() {
        while (unwind()->getNext()) {
            // do nothing
        }
    }
};

/** An empty collection produces no results. */
class Empty : public CheckResultsBase {};

/** A document without the unwind field produces no results. */
class MissingField : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {Document()};
    }
};

/** A document with a null field produces no results. */
class NullField : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("a" << BSONNULL)};
    }
};

/** A document with an empty array produces no results. */
class EmptyArray : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("a" << BSONArray())};
    }
};

/** Unwind an array with one value. */
class UnwindOneValue : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:1}]";
    }
};

/** Unwind an array with two values. */
class UnwindTwoValues : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:1},{_id:0,a:2}]";
    }
};

/** Unwind an array with two values, one of which is null. */
class UnwindNull : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << BSONNULL))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:1},{_id:0,a:null}]";
    }
};

/** Unwind two documents with arrays. */
class TwoDocuments : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2)),
                DOC("_id" << 1 << "a" << DOC_ARRAY(3 << 4))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:1},{_id:0,a:2},{_id:1,a:3},{_id:1,a:4}]";
    }
};

/** Unwind an array in a nested document. */
class NestedArray : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC("b" << DOC_ARRAY(1 << 2) << "c" << 3))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:{b:1,c:3}},{_id:0,a:{b:2,c:3}}]";
    }
    string unwindFieldPath() const {
        return "$a.b";
    }
};

/** A missing array (that cannot be nested below a non object field) produces no results. */
class NonObjectParent : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << 4)};
    }
    string unwindFieldPath() const {
        return "$a.b";
    }
};

/** Unwind an array in a doubly nested document. */
class DoubleNestedArray : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a"
                          << DOC("b" << DOC("d" << DOC_ARRAY(1 << 2) << "e" << 4) << "c" << 3))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:{b:{d:1,e:4},c:3}},{_id:0,a:{b:{d:2,e:4},c:3}}]";
    }
    string unwindFieldPath() const {
        return "$a.b.d";
    }
};

/** Unwind several documents in a row. */
class SeveralDocuments : public CheckResultsBase {
    std::deque<Document> inputData() {
        return {DOC("_id" << 0 << "a" << DOC_ARRAY(1 << 2 << 3)),
                DOC("_id" << 1),
                DOC("_id" << 2),
                DOC("_id" << 3 << "a" << DOC_ARRAY(10 << 20)),
                DOC("_id" << 4 << "a" << DOC_ARRAY(30))};
    }
    string expectedResultSetString() const {
        return "[{_id:0,a:1},{_id:0,a:2},{_id:0,a:3},{_id:3,a:10},"
               "{_id:3,a:20},{_id:4,a:30}]";
    }
};

/** Unwind several more documents in a row. */
class SeveralMoreDocuments : public CheckResultsBase {
    std::deque<Document> inputData() {
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
    string expectedResultSetString() const {
        return "[{_id:2,a:'a'},{_id:2,a:'b'},{_id:4,a:1},{_id:4,a:2},"
               "{_id:4,a:3},{_id:5,a:4},{_id:5,a:5},{_id:5,a:6},"
               "{_id:6,a:7},{_id:6,a:8},{_id:6,a:9}]";
    }
};

/** Dependant field paths. */
class Dependencies : public Base {
public:
    void run() {
        createUnwind("$x.y.z");
        DepsTracker dependencies;
        ASSERT_EQUALS(DocumentSource::SEE_NEXT, unwind()->getDependencies(&dependencies));
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("x.y.z"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

}  // namespace DocumentSourceUnwind

namespace DocumentSourceGeoNear {
using mongo::DocumentSourceGeoNear;
using mongo::DocumentSourceLimit;

class LimitCoalesce : public Mock::Base {
public:
    void run() {
        intrusive_ptr<DocumentSourceGeoNear> geoNear = DocumentSourceGeoNear::create(ctx());

        ASSERT_EQUALS(geoNear->getLimit(), 100);

        ASSERT(geoNear->coalesce(DocumentSourceLimit::create(ctx(), 200)));
        ASSERT_EQUALS(geoNear->getLimit(), 100);

        ASSERT(geoNear->coalesce(DocumentSourceLimit::create(ctx(), 50)));
        ASSERT_EQUALS(geoNear->getLimit(), 50);

        ASSERT(geoNear->coalesce(DocumentSourceLimit::create(ctx(), 30)));
        ASSERT_EQUALS(geoNear->getLimit(), 30);
    }
};
}  // namespace DocumentSourceGeoNear

namespace DocumentSourceMatch {
using mongo::DocumentSourceMatch;

// Helpers to make a DocumentSourceMatch from a query object or json string
intrusive_ptr<DocumentSourceMatch> makeMatch(const BSONObj& query) {
    intrusive_ptr<DocumentSource> uncasted =
        DocumentSourceMatch::createFromBson(BSON("$match" << query).firstElement(), NULL);
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

class Coalesce {
public:
    void run() {
        intrusive_ptr<DocumentSourceMatch> match1 = makeMatch(BSON("a" << 1));
        intrusive_ptr<DocumentSourceMatch> match2 = makeMatch(BSON("b" << 1));
        intrusive_ptr<DocumentSourceMatch> match3 = makeMatch(BSON("c" << 1));

        // Check initial state
        ASSERT_EQUALS(match1->getQuery(), BSON("a" << 1));
        ASSERT_EQUALS(match2->getQuery(), BSON("b" << 1));
        ASSERT_EQUALS(match3->getQuery(), BSON("c" << 1));

        ASSERT(match1->coalesce(match2));
        ASSERT_EQUALS(match1->getQuery(), fromjson("{'$and': [{a:1}, {b:1}]}"));

        ASSERT(match1->coalesce(match3));
        ASSERT_EQUALS(match1->getQuery(),
                      fromjson(
                          "{'$and': [{'$and': [{a:1}, {b:1}]},"
                          "{c:1}]}"));
    }
};
}  // namespace DocumentSourceMatch

class All : public Suite {
public:
    All() : Suite("documentsource") {}
    void setupTests() {
        add<DocumentSourceClass::Deps>();

        add<DocumentSourceLimit::DisposeSource>();
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

        add<DocumentSourceProject::Inclusion>();
        add<DocumentSourceProject::Optimize>();
        add<DocumentSourceProject::NonObjectSpec>();
        add<DocumentSourceProject::EmptyObjectSpec>();
        add<DocumentSourceProject::TopLevelDollar>();
        add<DocumentSourceProject::InvalidSpec>();
        add<DocumentSourceProject::TwoDocuments>();
        add<DocumentSourceProject::Dependencies>();

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

        add<DocumentSourceUnwind::Empty>();
        add<DocumentSourceUnwind::MissingField>();
        add<DocumentSourceUnwind::NullField>();
        add<DocumentSourceUnwind::EmptyArray>();
        add<DocumentSourceUnwind::UnwindOneValue>();
        add<DocumentSourceUnwind::UnwindTwoValues>();
        add<DocumentSourceUnwind::UnwindNull>();
        add<DocumentSourceUnwind::TwoDocuments>();
        add<DocumentSourceUnwind::NestedArray>();
        add<DocumentSourceUnwind::NonObjectParent>();
        add<DocumentSourceUnwind::DoubleNestedArray>();
        add<DocumentSourceUnwind::SeveralDocuments>();
        add<DocumentSourceUnwind::SeveralMoreDocuments>();
        add<DocumentSourceUnwind::Dependencies>();

        add<DocumentSourceGeoNear::LimitCoalesce>();

        add<DocumentSourceMatch::RedactSafePortion>();
        add<DocumentSourceMatch::Coalesce>();
    }
};

SuiteInstance<All> myall;

}  // namespace DocumentSourceTests
