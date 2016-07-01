/**
 *    Copyright (C) 2012 10gen Inc.
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
#include <string>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {
bool isMongos() {
    return false;
}
}

namespace PipelineTests {

using boost::intrusive_ptr;
using std::string;
using std::vector;

namespace Optimizations {
using namespace mongo;

namespace Local {
class Base {
public:
    // These return json arrays of pipeline operators
    virtual string inputPipeJson() = 0;
    virtual string outputPipeJson() = 0;

    // This returns a json array of pipeline operators, and is used to check that each pipeline
    // stage is serialized correctly (note: this is not the same as being explained correctly.)
    virtual string serializedPipeJson() {
        // serializedPipeJson should be equal to outputPipeJson if a stage's explain output is
        // parseable. An example of a stage that has unparseable explain output would be:
        // {$lookup: {..., unwinding: {...}}} instead of {$lookup: {...}}, {$unwind: {...}}.
        return outputPipeJson();
    }

    BSONObj pipelineFromJsonArray(const string& array) {
        return fromjson("{pipeline: " + array + "}");
    }
    virtual void run() {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson());
        const BSONObj outputPipeExpected = pipelineFromJsonArray(outputPipeJson());
        const BSONObj serializePipeExpected = pipelineFromJsonArray(serializedPipeJson());

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::Object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregationRequest request(NamespaceString("a.collection"), rawPipeline);
        intrusive_ptr<ExpressionContext> ctx = new ExpressionContext(&_opCtx, request);
        auto outputPipe = uassertStatusOK(Pipeline::parse(request.getPipeline(), ctx));
        outputPipe->optimizePipeline();

        ASSERT_VALUE_EQ(Value(outputPipe->writeExplainOps()),
                        Value(outputPipeExpected["pipeline"]));
        ASSERT_VALUE_EQ(Value(outputPipe->serialize()), Value(serializePipeExpected["pipeline"]));
    }

    virtual ~Base() {}

private:
    OperationContextNoop _opCtx;
};

class MoveSkipBeforeProject : public Base {
    string inputPipeJson() override {
        return "[{$project: {a : 1}}, {$skip : 5}]";
    }
    string outputPipeJson() override {
        return "[{$skip : 5}, {$project: {_id: true, a : true}}]";
    }
};

class MoveLimitBeforeProject : public Base {
    string inputPipeJson() override {
        return "[{$project: {a : 1}}, {$limit : 5}]";
    }

    string outputPipeJson() override {
        return "[{$limit : 5}, {$project: {_id: true, a : true}}]";
    }
};

class MoveMultipleSkipsAndLimitsBeforeProject : public Base {
    string inputPipeJson() override {
        return "[{$project: {a : 1}}, {$limit : 5}, {$skip : 3}]";
    }

    string outputPipeJson() override {
        return "[{$limit : 5}, {$skip : 3}, {$project: {_id: true, a : true}}]";
    }
};

class SkipSkipLimitBecomesLimitSkip : public Base {
    string inputPipeJson() override {
        return "[{$skip : 3}"
               ",{$skip : 5}"
               ",{$limit: 5}"
               "]";
    }

    string outputPipeJson() override {
        return "[{$limit: 13}"
               ",{$skip :  8}"
               "]";
    }
};

class SortMatchProjSkipLimBecomesMatchTopKSortSkipProj : public Base {
    string inputPipeJson() override {
        return "[{$sort: {a: 1}}"
               ",{$match: {a: 1}}"
               ",{$project : {a: 1}}"
               ",{$skip : 3}"
               ",{$limit: 5}"
               "]";
    }

    string outputPipeJson() override {
        return "[{$match: {a: 1}}"
               ",{$sort: {sortKey: {a: 1}, limit: 8}}"
               ",{$skip: 3}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }

    string serializedPipeJson() override {
        return "[{$match: {a: 1}}"
               ",{$sort: {a: 1}}"
               ",{$limit: 8}"
               ",{$skip : 3}"
               ",{$project : {_id: true, a: true}}"
               "]";
    }
};

class RemoveSkipZero : public Base {
    string inputPipeJson() override {
        return "[{$skip: 0}]";
    }

    string outputPipeJson() override {
        return "[]";
    }
};

class DoNotRemoveSkipOne : public Base {
    string inputPipeJson() override {
        return "[{$skip: 1}]";
    }

    string outputPipeJson() override {
        return "[{$skip: 1}]";
    }
};

class RemoveEmptyMatch : public Base {
    string inputPipeJson() override {
        return "[{$match: {}}]";
    }

    string outputPipeJson() override {
        return "[]";
    }
};

class RemoveMultipleEmptyMatches : public Base {
    string inputPipeJson() override {
        return "[{$match: {}}, {$match: {}}]";
    }

    string outputPipeJson() override {
        // TODO: The desired behavior here is to end up with an empty array.
        return "[{$match: {$and: [{}, {}]}}]";
    }
};

class DoNotRemoveNonEmptyMatch : public Base {
    string inputPipeJson() override {
        return "[{$match: {_id: 1}}]";
    }

    string outputPipeJson() override {
        return "[{$match: {_id: 1}}]";
    }
};

class MoveMatchBeforeSort : public Base {
    string inputPipeJson() override {
        return "[{$sort: {b: 1}}, {$match: {a: 2}}]";
    }

    string outputPipeJson() override {
        return "[{$match: {a: 2}}, {$sort: {sortKey: {b: 1}}}]";
    }

    string serializedPipeJson() override {
        return "[{$match: {a: 2}}, {$sort: {b: 1}}]";
    }
};

class LookupShouldCoalesceWithUnwindOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same'}}"
               "]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false}}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same'}}"
               "]";
    }
};

class LookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
               "]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
               "]";
    }
};

class LookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
               "]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: "
               "'index'}}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
               "]";
    }
};

class LookupShouldNotCoalesceWithUnwindNotOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
};

class LookupShouldSwapWithMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$match: {'independent': 0}}]";
    }
    string outputPipeJson() {
        return "[{$match: {independent: 0}}, "
               " {$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}]";
    }
};

class LookupShouldSplitMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$match: {'independent': 0, asField: {$eq: 3}}}]";
    }
    string outputPipeJson() {
        return "[{$match: {independent: {$eq: 0}}}, "
               " {$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$match: {asField: {$eq: 3}}}]";
    }
};
class LookupShouldNotAbsorbMatchOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$match: {'asField.subfield': 0}}]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$match: {'asField.subfield': 0}}]";
    }
};

class LookupShouldAbsorbUnwindMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               "{$unwind: '$asField'}, "
               "{$match: {'asField.subfield': {$eq: 1}}}]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z', "
               "            unwinding: {preserveNullAndEmptyArrays: false}, "
               "            matching: {subfield: {$eq: 1}}}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               "{$unwind: {path: '$asField'}}, "
               "{$match: {'asField.subfield': {$eq: 1}}}]";
    }
};

class LookupShouldAbsorbUnwindAndSplitAndAbsorbMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: '$asField'}, "
               " {$match: {'asField.subfield': {$eq: 1}, independentField: {$gt: 2}}}]";
    }
    string outputPipeJson() {
        return "[{$match: {independentField: {$gt: 2}}}, "
               " {$lookup: { "
               "      from: 'foo', "
               "      as: 'asField', "
               "      localField: 'y', "
               "      foreignField: 'z', "
               "      unwinding: { "
               "          preserveNullAndEmptyArrays: false"
               "      }, "
               "      matching: { "
               "          subfield: {$eq: 1} "
               "      } "
               " }}]";
    }
    string serializedPipeJson() {
        return "[{$match: {independentField: {$gt: 2}}}, "
               " {$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField'}}, "
               " {$match: {'asField.subfield': {$eq: 1}}}]";
    }
};

class LookupShouldNotSplitIndependentAndDependentOrClauses : public Base {
    // If any child of the $or is dependent on the 'asField', then the $match cannot be moved above
    // the $lookup, and if any child of the $or is independent of the 'asField', then the $match
    // cannot be absorbed by the $lookup.
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: '$asField'}, "
               " {$match: {$or: [{'independent': {$gt: 4}}, "
               "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    }
    string outputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z', "
               "            unwinding: {preserveNullAndEmptyArrays: false}}}, "
               " {$match: {$or: [{'independent': {$gt: 4}}, "
               "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField'}}, "
               " {$match: {$or: [{'independent': {$gt: 4}}, "
               "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    }
};

class LookupWithMatchOnArrayIndexFieldShouldNotCoalesce : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
               " {$match: {index: 0, 'asField.value': {$gt: 0}, independent: 1}}]";
    }
    string outputPipeJson() {
        return "[{$match: {independent: {$eq: 1}}}, "
               " {$lookup: { "
               "      from: 'foo', "
               "      as: 'asField', "
               "      localField: 'y', "
               "      foreignField: 'z', "
               "      unwinding: { "
               "          preserveNullAndEmptyArrays: false, "
               "          includeArrayIndex: 'index' "
               "      } "
               " }}, "
               " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    }
    string serializedPipeJson() {
        return "[{$match: {independent: {$eq: 1}}}, "
               " {$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
               " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    }
};

class LookupWithUnwindPreservingNullAndEmptyArraysShouldNotCoalesce : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
               " {$match: {'asField.value': {$gt: 0}, independent: 1}}]";
    }
    string outputPipeJson() {
        return "[{$match: {independent: {$eq: 1}}}, "
               " {$lookup: { "
               "      from: 'foo', "
               "      as: 'asField', "
               "      localField: 'y', "
               "      foreignField: 'z', "
               "      unwinding: { "
               "          preserveNullAndEmptyArrays: true"
               "      } "
               " }}, "
               " {$match: {'asField.value': {$gt: 0}}}]";
    }
    string serializedPipeJson() {
        return "[{$match: {independent: {$eq: 1}}}, "
               " {$lookup: {from: 'foo', as: 'asField', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
               " {$match: {'asField.value': {$gt: 0}}}]";
    }
};

class LookupDoesNotAbsorbElemMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: '$x'}, "
               " {$match: {x: {$elemMatch: {a: 1}}}}]";
    }
    string outputPipeJson() {
        return "[{$lookup: { "
               "             from: 'foo', "
               "             as: 'x', "
               "             localField: 'y', "
               "             foreignField: 'z', "
               "             unwinding: { "
               "                          preserveNullAndEmptyArrays: false "
               "             } "
               "           } "
               " }, "
               " {$match: {x: {$elemMatch: {a: 1}}}}]";
    }
    string serializedPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$x'}}, "
               " {$match: {x: {$elemMatch: {a: 1}}}}]";
    }
};

class LookupDoesSwapWithMatchOnLocalField : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$match: {y: {$eq: 3}}}]";
    }
    string outputPipeJson() {
        return "[{$match: {y: {$eq: 3}}}, "
               " {$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}]";
    }
};

class LookupDoesSwapWithMatchOnFieldWithSameNameAsForeignField : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$match: {z: {$eq: 3}}}]";
    }
    string outputPipeJson() {
        return "[{$match: {z: {$eq: 3}}}, "
               " {$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}]";
    }
};

class LookupDoesNotAbsorbUnwindOnSubfieldOfAsButStillMovesMatch : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$unwind: {path: '$x.subfield'}}, "
               " {$match: {'independent': 2, 'x.dependent': 2}}]";
    }
    string outputPipeJson() {
        return "[{$match: {'independent': {$eq: 2}}}, "
               " {$lookup: {from: 'foo', as: 'x', localField: 'y', foreignField: 'z'}}, "
               " {$match: {'x.dependent': {$eq: 2}}}, "
               " {$unwind: {path: '$x.subfield'}}]";
    }
};

class MatchShouldDuplicateItselfBeforeRedact : public Base {
    string inputPipeJson() {
        return "[{$redact: '$$PRUNE'}, {$match: {a: 1, b:12}}]";
    }
    string outputPipeJson() {
        return "[{$match: {a: 1, b:12}}, {$redact: '$$PRUNE'}, {$match: {a: 1, b:12}}]";
    }
};

class MatchShouldSwapWithUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: '$a.b.c'}, "
               "{$match: {'b': 1}}]";
    }
    string outputPipeJson() {
        return "[{$match: {'b': 1}}, "
               "{$unwind: {path: '$a.b.c'}}]";
    }
};

class MatchOnPrefixShouldNotSwapOnUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a.b.c'}}, "
               "{$match: {'a.b': 1}}]";
    }
    string outputPipeJson() {
        return "[{$unwind: {path: '$a.b.c'}}, "
               "{$match: {'a.b': 1}}]";
    }
};

class MatchShouldSplitOnUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: '$a.b'}, "
               "{$match: {$and: [{f: {$eq: 5}}, "
               "                 {$nor: [{'a.d': 1, c: 5}, {'a.b': 3, c: 5}]}]}}]";
    }
    string outputPipeJson() {
        return "[{$match: {$and: [{f: {$eq: 5}},"
               "                  {$nor: [{$and: [{'a.d': {$eq: 1}}, {c: {$eq: 5}}]}]}]}},"
               "{$unwind: {path: '$a.b'}}, "
               "{$match: {$nor: [{$and: [{'a.b': {$eq: 3}}, {c: {$eq: 5}}]}]}}]";
    }
};

class MatchShouldNotOptimizeWithElemMatch : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a.b'}}, "
               "{$match: {a: {$elemMatch: {b: {d: 1}}}}}]";
    }
    string outputPipeJson() {
        return "[{$unwind: {path: '$a.b'}}, "
               "{$match: {a: {$elemMatch: {b: {d: 1}}}}}]";
    }
};

class MatchShouldNotOptimizeWhenMatchingOnIndexField : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
               " {$match: {foo: 0, b: 1}}]";
    }
    string outputPipeJson() {
        return "[{$match: {b: {$eq: 1}}}, "
               " {$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
               " {$match: {foo: {$eq: 0}}}]";
    }
};

class MatchWithNorOnlySplitsIndependentChildren : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, "
               "{$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}, {b: {$eq: 2}} ]}}]";
    }
    string outputPipeJson() {
        return "[{$match: {$nor: [{b: {$eq: 2}}]}}, "
               "{$unwind: {path: '$a'}}, "
               "{$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}}]";
    }
};

class MatchWithOrDoesNotSplit : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, "
               "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    }
    string outputPipeJson() {
        return "[{$unwind: {path: '$a'}}, "
               "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    }
};

class UnwindBeforeDoubleMatchShouldRepeatedlyOptimize : public Base {
    string inputPipeJson() {
        return "[{$unwind: '$a'}, "
               "{$match: {b: {$gt: 0}}}, "
               "{$match: {a: 1, c: 1}}]";
    }
    string outputPipeJson() {
        return "[{$match: {$and: [{b: {$gt: 0}}, {c: {$eq: 1}}]}},"
               "{$unwind: {path: '$a'}}, "
               "{$match: {a: {$eq: 1}}}]";
    }
};

class GraphLookupShouldCoalesceWithUnwindOnAs : public Base {
    string inputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: '$out'}]";
    }

    string outputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d', unwinding: {preserveNullAndEmptyArrays: "
               "false}}}]";
    }

    string serializedPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$out'}}]";
    }
};

class GraphLookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty : public Base {
    string inputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";
    }

    string outputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    }

    string serializedPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";
    }
};

class GraphLookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex : public Base {
    string inputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";
    }

    string outputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d', unwinding: {preserveNullAndEmptyArrays: false, "
               "                                             includeArrayIndex: 'index'}}}]";
    }

    string serializedPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";
    }
};

class GraphLookupShouldNotCoalesceWithUnwindNotOnAs : public Base {
    string inputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: '$nottherightthing'}]";
    }

    string outputPipeJson() final {
        return "[{$graphLookup: {from: 'a', as: 'out', connectToField: 'b', connectFromField: 'c', "
               "                 startWith: '$d'}}, "
               " {$unwind: {path: '$nottherightthing'}}]";
    }
};

}  // namespace Local

namespace Sharded {
class Base {
public:
    // These all return json arrays of pipeline operators
    virtual string inputPipeJson() = 0;
    virtual string shardPipeJson() = 0;
    virtual string mergePipeJson() = 0;

    BSONObj pipelineFromJsonArray(const string& array) {
        return fromjson("{pipeline: " + array + "}");
    }
    virtual void run() {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson());
        const BSONObj shardPipeExpected = pipelineFromJsonArray(shardPipeJson());
        const BSONObj mergePipeExpected = pipelineFromJsonArray(mergePipeJson());

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::Object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregationRequest request(NamespaceString("a.collection"), rawPipeline);
        intrusive_ptr<ExpressionContext> ctx = new ExpressionContext(&_opCtx, request);
        mergePipe = uassertStatusOK(Pipeline::parse(request.getPipeline(), ctx));
        mergePipe->optimizePipeline();

        shardPipe = mergePipe->splitForSharded();
        ASSERT(shardPipe != nullptr);

        ASSERT_VALUE_EQ(Value(shardPipe->writeExplainOps()), Value(shardPipeExpected["pipeline"]));
        ASSERT_VALUE_EQ(Value(mergePipe->writeExplainOps()), Value(mergePipeExpected["pipeline"]));
    }

    virtual ~Base() {}

protected:
    intrusive_ptr<Pipeline> mergePipe;
    intrusive_ptr<Pipeline> shardPipe;

private:
    OperationContextNoop _opCtx;
};

// General test to make sure all optimizations support empty pipelines
class Empty : public Base {
    string inputPipeJson() {
        return "[]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[]";
    }
};

namespace moveFinalUnwindFromShardsToMerger {

class OneUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
};

class TwoUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]}";
    }
};

class UnwindNotFinal : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$match: {a:1}}]}";
    }
    string shardPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$match: {a:1}}]}";
    }
    string mergePipeJson() {
        return "[]}";
    }
};

class UnwindWithOther : public Base {
    string inputPipeJson() {
        return "[{$match: {a:1}}, {$unwind: {path: '$a'}}]}";
    }
    string shardPipeJson() {
        return "[{$match: {a:1}}]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
};
}  // namespace moveFinalUnwindFromShardsToMerger


namespace limitFieldsSentFromShardsToMerger {
// These tests use $limit to split the pipelines between shards and merger as it is
// always a split point and neutral in terms of needed fields.

class NeedWholeDoc : public Base {
    string inputPipeJson() {
        return "[{$limit:1}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}]";
    }
};

class JustNeedsId : public Base {
    string inputPipeJson() {
        return "[{$limit:1}, {$group: {_id: '$_id'}}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}, {$project: {_id:true}}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}, {$group: {_id: '$_id'}}]";
    }
};

class JustNeedsNonId : public Base {
    string inputPipeJson() {
        return "[{$limit:1}, {$group: {_id: '$a.b'}}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}, {$project: {_id: false, a: {b: true}}}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}, {$group: {_id: '$a.b'}}]";
    }
};

class NothingNeeded : public Base {
    string inputPipeJson() {
        return "[{$limit:1}"
               ",{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}"
               "]";
    }
    string shardPipeJson() {
        return "[{$limit:1}"
               ",{$project: {_id: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$limit:1}"
               ",{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}"
               "]";
    }
};

class ShardAlreadyExhaustive : public Base {
    // No new project should be added. This test reflects current behavior where the
    // 'a' field is still sent because it is explicitly asked for, even though it
    // isn't actually needed. If this changes in the future, this test will need to
    // change.
    string inputPipeJson() {
        return "[{$project: {_id:true, a:true}}"
               ",{$group: {_id: '$_id'}}"
               "]";
    }
    string shardPipeJson() {
        return "[{$project: {_id:true, a:true}}"
               ",{$group: {_id: '$_id'}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$group: {_id: '$$ROOT._id', $doingMerge: true}}"
               "]";
    }
};

class ShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj : public Base {
    string inputPipeJson() {
        return "[{$sort: {a : 1}}"
               ",{$match: {a: 1}}"
               ",{$project : {a: 1}}"
               ",{$skip : 3}"
               ",{$limit: 5}"
               "]";
    }
    string shardPipeJson() {
        return "[{$match: {a: 1}}"
               ",{$sort: {sortKey: {a: 1}, limit: 8}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: 1}, mergePresorted: true, limit: 8}}"
               ",{$skip: 3}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
};

}  // namespace limitFieldsSentFromShardsToMerger

namespace coalesceLookUpAndUnwind {

class ShouldCoalesceUnwindOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false}}}]";
    }
};

class ShouldCoalesceUnwindOnAsWithPreserveEmpty : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    }
};

class ShouldCoalesceUnwindOnAsWithIncludeArrayIndex : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: "
               "'index'}}}]";
    }
};

class ShouldNotCoalesceUnwindNotOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
};

}  // namespace coalesceLookUpAndUnwind

namespace needsPrimaryShardMerger {
class needsPrimaryShardMergerBase : public Base {
public:
    void run() override {
        Base::run();
        ASSERT_EQUALS(mergePipe->needsPrimaryShardMerger(), needsPrimaryShardMerger());
        ASSERT(!shardPipe->needsPrimaryShardMerger());
    }
    virtual bool needsPrimaryShardMerger() = 0;
};

class Out : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return true;
    }
    string inputPipeJson() {
        return "[{$out: 'outColl'}]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$out: 'outColl'}]";
    }
};

class Project : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return false;
    }
    string inputPipeJson() {
        return "[{$project: {a : 1}}]";
    }
    string shardPipeJson() {
        return "[{$project: {_id: true, a: true}}]";
    }
    string mergePipeJson() {
        return "[]";
    }
};

class LookUp : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return true;
    }
    string inputPipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'coll2', as : 'same', localField: 'left', foreignField: "
               "'right'}}]";
    }
};

}  // namespace needsPrimaryShardMerger
}  // namespace Sharded
}  // namespace Optimizations

namespace {

TEST(PipelineInitialSource, GeoNearInitialQuery) {
    OperationContextNoop _opCtx;
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$geoNear: {distanceField: 'd', near: [0, 0], query: {a: 1}}}")};
    intrusive_ptr<ExpressionContext> ctx = new ExpressionContext(
        &_opCtx, AggregationRequest(NamespaceString("a.collection"), rawPipeline));
    auto pipe = uassertStatusOK(Pipeline::parse(rawPipeline, ctx));
    ASSERT_EQ(pipe->getInitialQuery(), BSON("a" << 1));
}

TEST(PipelineInitialSource, MatchInitialQuery) {
    OperationContextNoop _opCtx;
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {'a': 4}}")};
    intrusive_ptr<ExpressionContext> ctx = new ExpressionContext(
        &_opCtx, AggregationRequest(NamespaceString("a.collection"), rawPipeline));

    auto pipe = uassertStatusOK(Pipeline::parse(rawPipeline, ctx));
    ASSERT_EQ(pipe->getInitialQuery(), BSON("a" << 4));
}

TEST(PipelineInitialSource, ParseCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], collation: {locale: 'reverse'}}");
    auto request = AggregationRequest::parseFromBSON(NamespaceString("a.collection"), inputBson);
    ASSERT_OK(request.getStatus());

    intrusive_ptr<ExpressionContext> ctx = new ExpressionContext(opCtx.get(), request.getValue());
    ASSERT(ctx->getCollator());
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_TRUE(CollatorInterface::collatorsMatch(ctx->getCollator(), &collator));
}

namespace Dependencies {

using PipelineDependenciesTest = AggregationContextFixture;

TEST_F(PipelineDependenciesTest, EmptyPipelineShouldRequireWholeDocument) {
    auto pipeline = unittest::assertGet(Pipeline::create({}, getExpCtx()));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedTextScore());

    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_TRUE(depsTracker.getNeedTextScore());
}

//
// Some dummy DocumentSources with different dependencies.
//

// Like a DocumentSourceMock, but can be used anywhere in the pipeline.
class DocumentSourceDependencyDummy : public DocumentSourceMock {
public:
    DocumentSourceDependencyDummy() : DocumentSourceMock({}) {}

    bool isValidInitialSource() const final {
        return false;
    }
};

class DocumentSourceDependenciesNotSupported : public DocumentSourceDependencyDummy {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return GetDepsReturn::NOT_SUPPORTED;
    }

    static boost::intrusive_ptr<DocumentSourceDependenciesNotSupported> create() {
        return new DocumentSourceDependenciesNotSupported();
    }
};

class DocumentSourceNeedsASeeNext : public DocumentSourceDependencyDummy {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return GetDepsReturn::SEE_NEXT;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsASeeNext> create() {
        return new DocumentSourceNeedsASeeNext();
    }
};

class DocumentSourceNeedsOnlyB : public DocumentSourceDependencyDummy {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return GetDepsReturn::EXHAUSTIVE_FIELDS;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyB> create() {
        return new DocumentSourceNeedsOnlyB();
    }
};

class DocumentSourceNeedsOnlyTextScore : public DocumentSourceDependencyDummy {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        deps->setNeedTextScore(true);
        return GetDepsReturn::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyTextScore> create() {
        return new DocumentSourceNeedsOnlyTextScore();
    }
};

class DocumentSourceStripsTextScore : public DocumentSourceDependencyDummy {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return GetDepsReturn::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceStripsTextScore> create() {
        return new DocumentSourceStripsTextScore();
    }
};

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfAnyStageDoesNotSupportDeps) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto notSupported = DocumentSourceDependenciesNotSupported::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext, notSupported}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    // The inputs did not have a text score available, so we should not require a text score.
    ASSERT_FALSE(depsTracker.getNeedTextScore());

    // Now in the other order.
    pipeline = unittest::assertGet(Pipeline::create({notSupported, needsASeeNext}, ctx));

    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfNoStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireWholeDocumentIfAnyStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext, needsOnlyB}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_EQ(depsTracker.fields.size(), 2UL);
    ASSERT_EQ(depsTracker.fields.count("a"), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotAddAnyRequiredFieldsAfterFirstStageWithExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsOnlyB, needsASeeNext}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedTextScore());

    // 'needsOnlyB' claims to know all its field dependencies, so we shouldn't add any from
    // 'needsASeeNext'.
    ASSERT_EQ(depsTracker.fields.size(), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfThereIsNoScoreAvailable) {
    auto ctx = getExpCtx();
    auto pipeline = unittest::assertGet(Pipeline::create({}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.getNeedTextScore());
}

TEST_F(PipelineDependenciesTest, ShouldThrowIfTextScoreIsNeededButNotPresent) {
    auto ctx = getExpCtx();
    auto needsText = DocumentSourceNeedsOnlyTextScore::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsText}, ctx));

    ASSERT_THROWS(pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata),
                  UserException);
}

TEST_F(PipelineDependenciesTest, ShouldRequireTextScoreIfAvailableAndNoStageReturnsExhaustiveMeta) {
    auto ctx = getExpCtx();
    auto pipeline = unittest::assertGet(Pipeline::create({}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.getNeedTextScore());

    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    pipeline = unittest::assertGet(Pipeline::create({needsASeeNext}, ctx));
    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.getNeedTextScore());
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfAvailableButDefinitelyNotNeeded) {
    auto ctx = getExpCtx();
    auto stripsTextScore = DocumentSourceStripsTextScore::create();
    auto needsText = DocumentSourceNeedsOnlyTextScore::create();
    auto pipeline = unittest::assertGet(Pipeline::create({stripsTextScore, needsText}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);

    // 'stripsTextScore' claims that no further stage will need metadata information, so we
    // shouldn't have the text score as a dependency.
    ASSERT_FALSE(depsTracker.getNeedTextScore());
}

}  // namespace Dependencies
}  // namespace

class All : public Suite {
public:
    All() : Suite("PipelineOptimizations") {}
    void setupTests() {
        add<Optimizations::Local::RemoveSkipZero>();
        add<Optimizations::Local::MoveLimitBeforeProject>();
        add<Optimizations::Local::MoveSkipBeforeProject>();
        add<Optimizations::Local::MoveMultipleSkipsAndLimitsBeforeProject>();
        add<Optimizations::Local::SkipSkipLimitBecomesLimitSkip>();
        add<Optimizations::Local::SortMatchProjSkipLimBecomesMatchTopKSortSkipProj>();
        add<Optimizations::Local::DoNotRemoveSkipOne>();
        add<Optimizations::Local::RemoveEmptyMatch>();
        add<Optimizations::Local::RemoveMultipleEmptyMatches>();
        add<Optimizations::Local::MoveMatchBeforeSort>();
        add<Optimizations::Local::DoNotRemoveNonEmptyMatch>();
        add<Optimizations::Local::LookupShouldCoalesceWithUnwindOnAs>();
        add<Optimizations::Local::LookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty>();
        add<Optimizations::Local::LookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex>();
        add<Optimizations::Local::LookupShouldNotCoalesceWithUnwindNotOnAs>();
        add<Optimizations::Local::LookupShouldSwapWithMatch>();
        add<Optimizations::Local::LookupShouldSplitMatch>();
        add<Optimizations::Local::LookupShouldNotAbsorbMatchOnAs>();
        add<Optimizations::Local::LookupShouldAbsorbUnwindMatch>();
        add<Optimizations::Local::LookupShouldAbsorbUnwindAndSplitAndAbsorbMatch>();
        add<Optimizations::Local::LookupShouldNotSplitIndependentAndDependentOrClauses>();
        add<Optimizations::Local::LookupWithMatchOnArrayIndexFieldShouldNotCoalesce>();
        add<Optimizations::Local::LookupWithUnwindPreservingNullAndEmptyArraysShouldNotCoalesce>();
        add<Optimizations::Local::LookupDoesNotAbsorbElemMatch>();
        add<Optimizations::Local::LookupDoesSwapWithMatchOnLocalField>();
        add<Optimizations::Local::LookupDoesNotAbsorbUnwindOnSubfieldOfAsButStillMovesMatch>();
        add<Optimizations::Local::LookupDoesSwapWithMatchOnFieldWithSameNameAsForeignField>();
        add<Optimizations::Local::GraphLookupShouldCoalesceWithUnwindOnAs>();
        add<Optimizations::Local::GraphLookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty>();
        add<Optimizations::Local::GraphLookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex>();
        add<Optimizations::Local::GraphLookupShouldNotCoalesceWithUnwindNotOnAs>();
        add<Optimizations::Local::MatchShouldDuplicateItselfBeforeRedact>();
        add<Optimizations::Local::MatchShouldSwapWithUnwind>();
        add<Optimizations::Local::MatchShouldNotOptimizeWhenMatchingOnIndexField>();
        add<Optimizations::Local::MatchOnPrefixShouldNotSwapOnUnwind>();
        add<Optimizations::Local::MatchShouldNotOptimizeWithElemMatch>();
        add<Optimizations::Local::MatchWithNorOnlySplitsIndependentChildren>();
        add<Optimizations::Local::MatchWithOrDoesNotSplit>();
        add<Optimizations::Local::MatchShouldSplitOnUnwind>();
        add<Optimizations::Local::UnwindBeforeDoubleMatchShouldRepeatedlyOptimize>();
        add<Optimizations::Sharded::Empty>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::ShouldCoalesceUnwindOnAs>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::
                ShouldCoalesceUnwindOnAsWithPreserveEmpty>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::
                ShouldCoalesceUnwindOnAsWithIncludeArrayIndex>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::ShouldNotCoalesceUnwindNotOnAs>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::OneUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::TwoUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindNotFinal>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindWithOther>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NeedWholeDoc>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsNonId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NothingNeeded>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::ShardAlreadyExhaustive>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::ShardAlreadyExhaustive>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::Out>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::Project>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::LookUp>();
    }
};

SuiteInstance<All> myall;

}  // namespace PipelineTests
