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

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {
bool isMongos() {
    return false;
}
}

namespace PipelineTests {

using boost::intrusive_ptr;
using std::string;

namespace Optimizations {
using namespace mongo;

namespace Local {
class Base {
public:
    // These both return json arrays of pipeline operators
    virtual string inputPipeJson() = 0;
    virtual string outputPipeJson() = 0;

    BSONObj pipelineFromJsonArray(const string& array) {
        return fromjson("{pipeline: " + array + "}");
    }
    virtual void run() {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson());
        const BSONObj outputPipeExpected = pipelineFromJsonArray(outputPipeJson());

        intrusive_ptr<ExpressionContext> ctx =
            new ExpressionContext(&_opCtx, NamespaceString("a.collection"));
        string errmsg;
        intrusive_ptr<Pipeline> outputPipe = Pipeline::parseCommand(errmsg, inputBson, ctx);
        ASSERT_EQUALS(errmsg, "");
        ASSERT(outputPipe != NULL);

        ASSERT_EQUALS(Value(outputPipe->writeExplainOps()), Value(outputPipeExpected["pipeline"]));
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
        return "[{$skip : 5}, {$project: {a : true}}]";
    }
};

class MoveLimitBeforeProject : public Base {
    string inputPipeJson() override {
        return "[{$project: {a : 1}}, {$limit : 5}]";
    }

    string outputPipeJson() override {
        return "[{$limit : 5}, {$project: {a : true}}]";
    }
};

class MoveMulitipleSkipsAndLimitsBeforeProject : public Base {
    string inputPipeJson() override {
        return "[{$project: {a : 1}}, {$limit : 5}, {$skip : 3}]";
    }

    string outputPipeJson() override {
        return "[{$limit : 5}, {$skip : 3}, {$project: {a : true}}]";
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
               ",{$project: {a: true}}"
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

        intrusive_ptr<ExpressionContext> ctx =
            new ExpressionContext(&_opCtx, NamespaceString("a.collection"));
        string errmsg;
        intrusive_ptr<Pipeline> mergePipe = Pipeline::parseCommand(errmsg, inputBson, ctx);
        ASSERT_EQUALS(errmsg, "");
        ASSERT(mergePipe != NULL);

        intrusive_ptr<Pipeline> shardPipe = mergePipe->splitForSharded();
        ASSERT(shardPipe != NULL);

        ASSERT_EQUALS(Value(shardPipe->writeExplainOps()), Value(shardPipeExpected["pipeline"]));
        ASSERT_EQUALS(Value(mergePipe->writeExplainOps()), Value(mergePipeExpected["pipeline"]));
    }

    virtual ~Base() {}

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
        return "[{$unwind: '$a'}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: '$a'}]}";
    }
};

class TwoUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: '$a'}, {$unwind: '$b'}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: '$a'}, {$unwind: '$b'}]}";
    }
};

class UnwindNotFinal : public Base {
    string inputPipeJson() {
        return "[{$unwind: '$a'}, {$match: {a:1}}]}";
    }
    string shardPipeJson() {
        return "[{$unwind: '$a'}, {$match: {a:1}}]}";
    }
    string mergePipeJson() {
        return "[]}";
    }
};

class UnwindWithOther : public Base {
    string inputPipeJson() {
        return "[{$match: {a:1}}, {$unwind: '$a'}]}";
    }
    string shardPipeJson() {
        return "[{$match: {a:1}}]}";
    }
    string mergePipeJson() {
        return "[{$unwind: '$a'}]}";
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

class JustNeedsMetadata : public Base {
    // Currently this optimization doesn't handle metadata and the shards assume it
    // needs to be propagated implicitly. Therefore the $project produced should be
    // the same as in NothingNeeded.
    string inputPipeJson() {
        return "[{$limit:1}, {$project: {_id: false, a: {$meta: 'textScore'}}}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}, {$project: {_id: true}}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}, {$project: {_id: false, a: {$meta: 'textScore'}}}]";
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
               ",{$project: {a: true, _id: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: 1}, mergePresorted: true, limit: 8}}"
               ",{$skip: 3}"
               ",{$project: {a: true}}"
               "]";
    }
};

}  // namespace limitFieldsSentFromShardsToMerger
}  // namespace Sharded
}  // namespace Optimizations

class All : public Suite {
public:
    All() : Suite("pipeline") {}
    void setupTests() {
        add<Optimizations::Local::RemoveSkipZero>();
        add<Optimizations::Local::MoveLimitBeforeProject>();
        add<Optimizations::Local::MoveSkipBeforeProject>();
        add<Optimizations::Local::MoveMulitipleSkipsAndLimitsBeforeProject>();
        add<Optimizations::Local::SortMatchProjSkipLimBecomesMatchTopKSortSkipProj>();
        add<Optimizations::Local::DoNotRemoveSkipOne>();
        add<Optimizations::Local::RemoveEmptyMatch>();
        add<Optimizations::Local::RemoveMultipleEmptyMatches>();
        add<Optimizations::Local::DoNotRemoveNonEmptyMatch>();
        add<Optimizations::Sharded::Empty>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::OneUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::TwoUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindNotFinal>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindWithOther>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NeedWholeDoc>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsNonId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NothingNeeded>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsMetadata>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::ShardAlreadyExhaustive>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj>();
    }
};

SuiteInstance<All> myall;

}  // namespace PipelineTests
