// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/streaming_agg.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/document_tree_generator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "fuzztest/googletest_fixture_adapter.h"

/*
 * Fuzz test that runs SBE's hash group stage (HashAggStage) and streaming group stage
 * (StreamingAggStage) on fuzzer-generated documents and grouping keys and asserts that both produce
 * the same groups with the same accumulator outputs.
 */

namespace mongo::sbe {
namespace {

// Vector of documents that will be the input to the group stages. Each document in the vector is
// rendered as {_id:i, ...fields in document...}
auto documentSpecDomain() {
    // each option has equal probability and fuzztest starts with the smallest size for each
    return fuzztest::OneOf(fuzztest::VectorOf(documentNodesDomain()).WithMinSize(9),
                           fuzztest::VectorOf(documentNodesDomain()).WithMinSize(1025));
}

/**
 * A ref in the fuzzer-generated group key, names one of the generated document's fields by index.
 */
using KeyRef = unsigned;

/**
 * Represents a scalar grouping key like {$group: {_id: "$ref"}}
 */
struct ScalarKey {
    KeyRef ref;
    bool operator==(const ScalarKey&) const = default;
};

/**
 * Represents an object grouping key like {$group: {_id: {ref1: "$ref1", ...}}
 * All the refs should be unique and correspond the key slots given to the grouping stage.
 */
struct ObjectKey {
    std::vector<KeyRef> refs;
    bool operator==(const ObjectKey&) const = default;
};

using KeySpec = std::variant<ScalarKey, ObjectKey>;

// Upper bound on how many field references an object group key may hold
constexpr unsigned kMaxKeyFields = 3;

auto keySpecDomain() {
    auto refDomain = fuzztest::InRange<KeyRef>(0, kMaxTreeBranching - 1);
    auto refsDomain = fuzztest::UniqueElementsVectorOf(refDomain).WithMaxSize(kMaxKeyFields);
    return fuzztest::VariantOf(fuzztest::StructOf<ScalarKey>(refDomain),
                               fuzztest::StructOf<ObjectKey>(refsDomain));
}

/*
 * The field references making up the group key, one per key slot.
 */
std::vector<KeyRef> keyRefs(const KeySpec& key) {
    if (const auto* objectKey = std::get_if<ObjectKey>(&key)) {
        return objectKey->refs;
    }
    return {std::get<ScalarKey>(key).ref};
}

// These functions are here so that ADL works properly in order to print the fuzzer-generated
// documents as JSON whenever the fuzzer finds a failure.
template <typename Sink>
void AbslStringify(Sink& sink, const ScalarKey& key) {
    absl::Format(&sink, "\"%s\"", positionalName(key.ref));
}

template <typename Sink>
void AbslStringify(Sink& sink, const ObjectKey& key) {
    absl::Format(&sink, "{");
    for (size_t slot = 0; slot < key.refs.size(); ++slot) {
        absl::Format(&sink,
                     "%s\"k%zu\": \"%s\"",
                     slot > 0 ? ", " : "",
                     slot,
                     positionalName(key.refs[slot]));
    }
    absl::Format(&sink, "}");
}

/**
 * Orders group keys for comparing stage outputs. Any ordering works here as long as it's
 * deterministic; whole rows compare lexicographically with the key slots first, so key ties cannot
 * hide a difference in the accumulated values.
 */
std::vector<std::pair<value::TypeTags, value::Value>> arrayToSortedRows(value::TypeTags tag,
                                                                        value::Value val) {
    std::vector<std::pair<value::TypeTags, value::Value>> rows;
    for (value::ArrayEnumerator it(tag, val); !it.atEnd(); it.advance()) {
        rows.push_back(it.getViewOfValue());
    }
    std::sort(rows.begin(), rows.end(), [](auto&& lhs, auto&& rhs) {
        auto [cmpTag, cmpVal] = value::compareValue(lhs.first, lhs.second, rhs.first, rhs.second);
        return cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) < 0;
    });
    return rows;
}

/*
 * Builds the scan input for both group stages: one row per document, as sbe arrays holding just the
 * document itself. Returns the rows sorted by the documents' group key fields, with missing fields
 * ordered as null, so that every distinct grouping key occupies a contiguous run of rows, as
 * StreamingAggStage's run-detection requires.
 */
value::TagValueOwned makeSortedInput(const std::vector<DocumentSpec>& docs, const KeySpec& key) {
    const auto refs = keyRefs(key);

    struct SortRow {
        // Declare a user constructor so that a default constructor isn't implicitly declared.
        // A strange memory error is possible where stable_sort doesn't detect implicitly-deleted
        // constructors, and allocates a buffer of uninitialized SortRow values, causing a memory
        // error when those values are released.
        SortRow(value::TagValueOwned key, value::TagValueOwned document)
            : sortKey(std::move(key)), doc(std::move(document)) {}

        value::TagValueOwned sortKey;  // array of key refs, used for sorting the rows
        value::TagValueOwned doc;
    };
    std::vector<SortRow> rows;
    rows.reserve(docs.size());

    for (size_t i = 0; i < docs.size(); ++i) {
        BSONObj doc = renderDocument(docs[i], static_cast<long long>(i));

        // Store the key field values in an array for sorting. This is only valid as long as there
        // are no Nothing values stored, since Nothing is silently dropped from SBE arrays.
        auto sortKey = std::make_unique<value::Array>();
        for (const auto& ref : refs) {
            auto elem = doc[positionalName(ref)];
            // For now use a string placeholder (len > 16 to avoid collisions with scalar strings)
            // as the sort key for Nothing, to sort missing separately from null, except in the case
            // where nothing will be rewritten to null in runGroupStage()
            // TODO(SERVER-136257) once null/missing disambiguation is implemented, always use
            // value::TagValueOwned::fromRaw(value::TypeTags::Null, 0) to sort null/missing equally
            auto getMissingSortKey = [&]() {
                if (refs.size() == 1)
                    return value::TagValueOwned::fromRaw(value::TypeTags::Null, 0);
                return value::TagValueOwned::fromRaw(
                    value::makeNewString("$$missingmissingmissing$$"));
            };
            sortKey->push_back(elem.eoo() ? getMissingSortKey() : bson::convertToOwned(elem));
        }
        auto [docTag, docVal] = stage_builder::makeValue(doc);
        rows.emplace_back(
            value::TagValueOwned::fromRaw(value::TypeTags::Array,
                                          reinterpret_cast<value::Value>(sortKey.release())),
            value::TagValueOwned::fromRaw(docTag, docVal));
    }

    // Sort the rows by keys, keeping rows with keys that compare equal in input order
    std::stable_sort(rows.begin(), rows.end(), [](const SortRow& lhs, const SortRow& rhs) {
        auto [cmpTag, cmpVal] = value::compareValue(
            lhs.sortKey.tag(), lhs.sortKey.value(), rhs.sortKey.tag(), rhs.sortKey.value());
        return cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) < 0;
    });

    auto arr = std::make_unique<value::Array>();
    for (auto& row : rows) {
        auto scanRow = std::make_unique<value::Array>();
        scanRow->push_back(std::move(row.doc));
        arr->push_back(value::TagValueOwned::fromRaw(
            value::TypeTags::Array, reinterpret_cast<value::Value>(scanRow.release())));
    }
    return value::TagValueOwned::fromRaw(value::TypeTags::Array,
                                         reinterpret_cast<value::Value>(arr.release()));
}

}  // namespace

class GroupStagesFuzzTest : public PlanStageTestFixture {
public:
    // PlanStageTestFixture enables spilling by default, override to disable
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions();
    }

    // TODO SERVER-129585: Remove this once fuzztests run the same initialization as unit tests
    static void SetUpTestSuite() {
        TestingProctor::instance().setEnabled(true);
        runGlobalInitializersOrDie({});
    }

    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    /*
     * Runs one group stage over 'input' (an array of single-element [doc] rows sorted by key) and
     * returns a deep copy of its output.
     */
    value::TagValueOwned runGroupStage(value::TagValueOwned input,
                                       const std::vector<KeyRef>& refs,
                                       bool streaming) {
        auto [scanSlots, splitScanStage] = generateVirtualScanMulti(1, std::move(input));
        const auto docSlot = scanSlots[0];

        // Project the group key fields out of the document using getField
        SlotExprPairVector keyFields;
        value::SlotVector keySlots;
        for (size_t i = 0; i < refs.size(); ++i) {
            auto keySlot = generateSlotId();
            auto keyExpr = makeFunction(EFn::kGetField,
                                        makeE<EVariable>(docSlot),
                                        makeStringConstant(positionalName(refs[i])));
            // Scalar and single-field object keys convert Nothing to Null
            if (refs.size() == 1) {
                keyExpr = makeE<EPrimBinary>(EPrimBinary::fillEmpty,
                                             std::move(keyExpr),
                                             makeE<EConstant>(value::TypeTags::Null, 0));
            }
            keyFields.emplace_back(keySlot, std::move(keyExpr));
            keySlots.push_back(keySlot);
        }
        auto scanStage =
            makeS<ProjectStage>(std::move(splitScanStage), std::move(keyFields), kEmptyPlanNodeId);

        const auto minSlot = generateSlotId();
        const auto maxSlot = generateSlotId();
        const auto firstSlot = generateSlotId();
        const auto lastSlot = generateSlotId();
        const auto minSpillSlot = generateSlotId();
        const auto maxSpillSlot = generateSlotId();
        const auto firstSpillSlot = generateSlotId();
        const auto lastSpillSlot = generateSlotId();

        auto accumulators = makeHashAggAccumulatorList(
            std::make_unique<CompiledHashAggAccumulator>(
                minSlot,
                minSpillSlot,
                makeFunction(EFn::kMin, makeE<EVariable>(docSlot)),
                makeFunction(EFn::kMin, makeE<EVariable>(minSpillSlot))),
            std::make_unique<CompiledHashAggAccumulator>(
                maxSlot,
                maxSpillSlot,
                makeFunction(EFn::kMax, makeE<EVariable>(docSlot)),
                makeFunction(EFn::kMax, makeE<EVariable>(maxSpillSlot))),
            std::make_unique<CompiledHashAggAccumulator>(
                firstSlot,
                firstSpillSlot,
                makeFunction(EFn::kFirst, makeE<EVariable>(docSlot)),
                makeFunction(EFn::kFirst, makeE<EVariable>(firstSpillSlot))),
            std::make_unique<CompiledHashAggAccumulator>(
                lastSlot,
                lastSpillSlot,
                makeFunction(EFn::kLast, makeE<EVariable>(docSlot)),
                makeFunction(EFn::kLast, makeE<EVariable>(lastSpillSlot))));

        std::unique_ptr<PlanStage> stage;
        if (streaming) {
            stage = makeS<StreamingAggStage>(std::move(scanStage),
                                             keySlots,
                                             boost::none,
                                             std::move(accumulators),
                                             kEmptyPlanNodeId);
        } else {
            stage = makeS<HashAggStage>(std::move(scanStage),
                                        keySlots,
                                        std::move(accumulators),
                                        true,
                                        boost::none,
                                        false /* allowDiskUse */,
                                        nullptr /* yieldPolicy */,
                                        kEmptyPlanNodeId);
        }

        // Replace missing key slots with a placeholder so the output rows are comparable
        SlotExprPairVector outFields;
        value::SlotVector slots;
        for (const auto keySlot : keySlots) {
            auto outSlot = generateSlotId();
            outFields.emplace_back(outSlot,
                                   makeE<EPrimBinary>(EPrimBinary::fillEmpty,
                                                      makeE<EVariable>(keySlot),
                                                      makeStringConstant("missing")));
            slots.push_back(outSlot);
        }

        auto ctx = makeCompileCtx();
        for (const auto accSlot : {minSlot, maxSlot, firstSlot, lastSlot}) {
            slots.push_back(accSlot);
        }
        auto root = makeS<ProjectStage>(std::move(stage), std::move(outFields), kEmptyPlanNodeId);
        auto accessors = prepareTree(ctx.get(), root.get(), slots);
        auto [tag, val] = getAllResultsMulti(root.get(), accessors);
        // getAllResultsMulti hands ownership of the result array to the caller; copy it and
        // release the original so fuzzing iterations do not accumulate leaked arrays.
        auto result = value::TagValueOwned::fromRaw(value::copyValue(tag, val));
        value::releaseValue(tag, val);
        return result;
    }

private:
    // Never run by gtest; this fixture is driven directly by the fuzz adapter.
    void TestBody() override {}

    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

/*
 * Fuzz property: on any generated set of documents, with a generated scalar or object grouping key,
 * the hash group stage and the streaming group stage produce identical groups with identical
 * min/max/first/last values.
 */
class GroupStagesFuzzAdapter : public fuzztest::PerFuzzTestFixtureAdapter<GroupStagesFuzzTest> {
public:
    void FuzzGroupStagesAgree(std::vector<DocumentSpec> docs, KeySpec key) {
        value::TagValueOwned input = makeSortedInput(docs, key);
        const auto refs = keyRefs(key);

        auto hashResult = runGroupStage(input.copy(), refs, false /* streaming */);
        auto streamResult = runGroupStage(std::move(input), refs, true /* streaming */);

        auto hashRows = arrayToSortedRows(hashResult.tag(), hashResult.value());
        auto streamRows = arrayToSortedRows(streamResult.tag(), streamResult.value());

        // Failure diagnostics: what each stage produced. Built unconditionally because gtest
        // evaluates the streamed message eagerly, but only printed on failure.
        auto printRows = [](const std::vector<std::pair<value::TypeTags, value::Value>>& rows) {
            std::string str = "[";
            for (size_t i = 0; i < rows.size(); ++i) {
                if (i > 0) {
                    str += ", ";
                }
                str += value::print(rows[i]);
            }
            return str + "]";
        };
        const auto stageOutputsMsg = "\n  hash group output: " + printRows(hashRows) +
            "\nstream group output: " + printRows(streamRows);

        ASSERT_EQ(hashRows.size(), streamRows.size()) << stageOutputsMsg;
        for (size_t i = 0; i < hashRows.size(); ++i) {
            auto [cmpTag, cmpVal] = value::compareValue(
                hashRows[i].first, hashRows[i].second, streamRows[i].first, streamRows[i].second);
            ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32) << "row " << i << stageOutputsMsg;
            ASSERT_EQ(cmpVal, 0) << "row " << i << stageOutputsMsg;
        }
    }
};

FUZZ_TEST_F(GroupStagesFuzzAdapter, FuzzGroupStagesAgree)
    .WithDomains(documentSpecDomain(), keySpecDomain());

}  // namespace mongo::sbe
