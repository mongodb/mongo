// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Measures the per-document cost of evaluating a filter against WorkingSetMembers, which is the
 * work done by every classic execution stage that has a filter.
 *
 * The cases are chosen to isolate where the cost lives:
 *   - NoFilter          null MatchExpression; measures the short-circuit path only.
 *   - TopLevelEq        single equality on a top-level field; no dotted path, so the iterator's
 *                       'restOfPath' string stays empty.
 *   - DottedEq          single equality on "a.b"; 'restOfPath' is populated on every document.
 *   - LongDottedEq      dotted path long enough to exceed the std::string small-string
 *                       optimization, so 'restOfPath' heap-allocates.
 *   - AndOfTwo          $and of a dotted and a top-level equality; two iterators per document.
 *   - ArrayDottedEq     "a.b" where 'a' is an array of subdocuments, which is the only case that
 *                       allocates a sub-iterator.
 *
 * Wallclock numbers here are small and easily swamped by machine noise. For A/B comparisons prefer
 * instruction counts:
 *   valgrind --tool=callgrind --callgrind-out-file=cg.out \
 *       bazel-bin/src/mongo/db/exec/classic/filter_bm \
 *       --benchmark_filter='DottedEq' --benchmark_min_time=0.01s
 *   callgrind_annotate cg.out | head -40
 */

#include "mongo/db/exec/classic/filter.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/storage/snapshot.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

constexpr int kNumDocuments = 4000;

// Long enough that std::string cannot store it inline.
constexpr std::string_view kLongPathPrefix = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kLongPathLeaf = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

/**
 * Owns a WorkingSet plus 'kNumDocuments' members in the OWNED_OBJ state, so that hasObj() is true
 * and filter evaluation takes the document path rather than the index-key path.
 */
class Documents {
public:
    explicit Documents(const std::function<BSONObj(int)>& generator) {
        _ids.reserve(kNumDocuments);
        for (int i = 0; i < kNumDocuments; ++i) {
            WorkingSetID id = _ws.allocate();
            WorkingSetMember* member = _ws.get(id);
            member->doc = {SnapshotId(), Document{generator(i)}};
            _ws.transitionToOwnedObj(id);
            _ids.push_back(id);
        }
    }

    WorkingSetMember* member(size_t i) {
        return _ws.get(_ids[i]);
    }

    size_t size() const {
        return _ids.size();
    }

private:
    WorkingSet _ws;
    std::vector<WorkingSetID> _ids;
};

BSONObj flatDoc(int i) {
    return BSON("state" << (i % 2 ? "NV" : "CT") << "amount" << i << "a" << BSON("b" << i % 97));
}

BSONObj longPathDoc(int i) {
    return BSON("state" << (i % 2 ? "NV" : "CT") << std::string{kLongPathPrefix}
                        << BSON(std::string{kLongPathLeaf} << i % 97));
}

BSONObj arrayDoc(int i) {
    BSONArrayBuilder arr;
    for (int j = 0; j < 4; ++j) {
        arr.append(BSON("b" << (i + j) % 97));
    }
    return BSON("state" << (i % 2 ? "NV" : "CT") << "amount" << i << "a" << arr.arr());
}

std::unique_ptr<MatchExpression> eq(std::string_view path, Value v) {
    return std::make_unique<EqualityMatchExpression>(path, std::move(v));
}

/**
 * Runs 'filter' over every document, reusing one Filter so that any per-stage state it holds is
 * reused across documents exactly as a real stage would reuse it.
 */
void runCase(benchmark::State& state,
             Documents& docs,
             const std::function<std::unique_ptr<MatchExpression>()>& makeFilter) {
    std::unique_ptr<MatchExpression> expr = makeFilter();

    for (auto keepRunning : state) {
        for (size_t i = 0; i < docs.size(); ++i) {
            benchmark::DoNotOptimize(Filter::passes(docs.member(i), expr.get()));
        }
    }
    state.SetItemsProcessed(state.iterations() * docs.size());
}

void BM_NoFilter(benchmark::State& state) {
    Documents docs(flatDoc);
    runCase(state, docs, [] { return nullptr; });
}

void BM_TopLevelEq(benchmark::State& state) {
    Documents docs(flatDoc);
    runCase(state, docs, [] { return eq("state", Value(std::string_view{"NV"})); });
}

void BM_DottedEq(benchmark::State& state) {
    Documents docs(flatDoc);
    runCase(state, docs, [] { return eq("a.b", Value(42)); });
}

void BM_LongDottedEq(benchmark::State& state) {
    Documents docs(longPathDoc);
    std::string path = std::string{kLongPathPrefix} + "." + std::string{kLongPathLeaf};
    runCase(state, docs, [&] { return eq(path, Value(42)); });
}

void BM_AndOfTwo(benchmark::State& state) {
    Documents docs(flatDoc);
    runCase(state, docs, [] {
        std::vector<std::unique_ptr<MatchExpression>> children;
        children.push_back(eq("a.b", Value(42)));
        children.push_back(eq("state", Value(std::string_view{"NV"})));
        return std::make_unique<AndMatchExpression>(std::move(children));
    });
}

void BM_ArrayDottedEq(benchmark::State& state) {
    Documents docs(arrayDoc);
    runCase(state, docs, [] { return eq("a.b", Value(42)); });
}

BENCHMARK(BM_NoFilter);
BENCHMARK(BM_TopLevelEq);
BENCHMARK(BM_DottedEq);
BENCHMARK(BM_LongDottedEq);
BENCHMARK(BM_AndOfTwo);
BENCHMARK(BM_ArrayDottedEq);

}  // namespace
}  // namespace mongo
