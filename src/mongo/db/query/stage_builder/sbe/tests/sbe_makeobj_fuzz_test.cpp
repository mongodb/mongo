// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/document_tree_generator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/stage_builder/sbe/abt/explain.h"
#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt_lower.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/testing_proctor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo::stage_builder {
namespace {
using sbe::MakeObjSpec;
using sbe::value::TagValueOwned;
using sbe::value::TypeTags;

constexpr size_t kMaxValueNodes = 16;
constexpr int kNumFieldNames = 6;
constexpr size_t kMaxChainLength = 8;

// FuzzTest's main does not run MongoDB's global initializers.
class MongoEnvironment : public testing::Environment {
    void SetUp() override {
        TestingProctor::instance().setEnabled(true);
        runGlobalInitializersOrDie({});
    }
};

[[maybe_unused]] const auto kMongoEnvironment =
    testing::AddGlobalTestEnvironment(new MongoEnvironment);

struct ObjectValue {
    sbe::DocumentSpec fields;
    bool bson;
    bool reverse;
};

struct ArrayValue {
    sbe::DocumentSpec elements;
};

using ValueSpec = std::variant<sbe::ScalarSpec, ObjectValue, ArrayValue>;

struct WriteSpec {
    ValueSpec value;
    bool slot;
    bool add;
};

using ActionSpec = std::variant<MakeObjSpec::Keep, MakeObjSpec::Drop, WriteSpec>;

struct CallSpec {
    // Each position names a0 through a5. An absent action leaves the field unlisted.
    std::array<std::optional<ActionSpec>, kNumFieldNames> actions;
    std::vector<int> fieldOrder;
    bool closed;
};

struct ChainSpec {
    ValueSpec input;
    std::vector<CallSpec> calls;
    bool bsonTop;
    MakeObjSpec::NonObjInputBehavior topBehavior;
    std::optional<int32_t> topDepth;
};

auto valueDomain() {
    auto document = fuzztest::VectorOf(sbe::nodeSpecDomain()).WithMaxSize(kMaxValueNodes);
    // ArraySets can change iteration order when copied, which becomes array order in BSON.
    // Exclude them until comparison can handle sets without ignoring array or object field order.
    return fuzztest::VariantOf(sbe::scalarSpecDomain(),
                               fuzztest::StructOf<ObjectValue>(document,
                                                               fuzztest::Arbitrary<bool>(),
                                                               fuzztest::Arbitrary<bool>()),
                               fuzztest::StructOf<ArrayValue>(document));
}

auto callDomain() {
    auto action = fuzztest::VariantOf(fuzztest::Just(MakeObjSpec::Keep{}),
                                      fuzztest::Just(MakeObjSpec::Drop{}),
                                      fuzztest::StructOf<WriteSpec>(valueDomain(),
                                                                    fuzztest::Arbitrary<bool>(),
                                                                    fuzztest::Arbitrary<bool>()));
    auto fieldOrder = fuzztest::UniqueElementsVectorOf(fuzztest::InRange(0, kNumFieldNames - 1))
                          .WithSize(kNumFieldNames);
    return fuzztest::StructOf<CallSpec>(
        fuzztest::ArrayOf<kNumFieldNames>(fuzztest::OptionalOf(action)),
        fieldOrder,
        fuzztest::Arbitrary<bool>());
}

auto chainDomain() {
    using Behavior = MakeObjSpec::NonObjInputBehavior;
    return fuzztest::StructOf<ChainSpec>(
        valueDomain(),
        fuzztest::VectorOf(callDomain()).WithMinSize(2).WithMaxSize(kMaxChainLength),
        fuzztest::Arbitrary<bool>(),
        fuzztest::ElementOf({Behavior::kNewObj, Behavior::kReturnInput, Behavior::kReturnNothing}),
        fuzztest::OptionalOf(fuzztest::InRange<int32_t>(0, 2)));
}

TagValueOwned makeValue(const ValueSpec& spec) {
    return std::visit(
        OverloadedVisitor{[](const sbe::ScalarSpec& v) { return sbe::renderScalarValue(v); },
                          [](const ObjectValue& v) {
                              auto bson = sbe::renderDocumentFields(v.fields);
                              if (v.reverse) {
                                  std::vector<BSONElement> elements;
                                  for (const auto& element : bson) {
                                      elements.push_back(element);
                                  }
                                  BSONObjBuilder builder;
                                  for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
                                      builder.append(*it);
                                  }
                                  bson = builder.obj();
                              }
                              return v.bson ? sbe::renderBsonObjectValue(bson)
                                            : sbe::renderObjectValue(bson);
                          },
                          [](const ArrayValue& v) {
                              auto elements = sbe::renderDocumentFields(v.elements);
                              return sbe::renderArrayValue(elements);
                          }},
        spec);
}

TEST(MakeObjValueTest, ArrayValuesPreserveOrderAndDuplicates) {
    auto array = makeValue(ArrayValue{
        {sbe::ScalarSpec{int32_t{2}}, sbe::ScalarSpec{int32_t{1}}, sbe::ScalarSpec{int32_t{2}}}});
    ASSERT_EQ(array.tag(), TypeTags::Array);
    BSONObjBuilder builder;
    sbe::bson::appendValueToBsonObj(builder, "array", array.tag(), array.value());
    EXPECT_TRUE(BSON("array" << BSON_ARRAY(2 << 1 << 2)).binaryEqual(builder.obj()));
}

abt::ABT nestCalls(const std::vector<abt::ABT>& calls) {
    auto result = calls.front();
    for (size_t i = 1; i < calls.size(); ++i) {
        auto next = calls[i];
        next.cast<abt::FunctionCall>()->nodes()[MakeObjSpec::kInputArgPos] = std::move(result);
        result = std::move(next);
    }
    return result;
}

// System under test. For now, return the chain unchanged.
abt::ABT test(const std::vector<abt::ABT>& calls) {
    return nestCalls(calls);
}

// Reference implementation. Return the original chain without rewrites.
abt::ABT oracle(const std::vector<abt::ABT>& calls) {
    return nestCalls(calls);
}

// All slots and compiled expressions belong to one input and are released between iterations.
class MakeObjCase {
    using FieldAction = MakeObjSpec::FieldAction;

    struct CallFields {
        std::vector<std::string> names;
        std::vector<FieldAction> actions;
        abt::ABTVector args;
    };

public:
    std::vector<abt::ABT> makeCalls(const ChainSpec& chain) {
        // Bind the bottom input once so the candidate and reference read the same value.
        auto input = bindValue(makeValue(chain.input), /*slot=*/true);
        std::vector<abt::ABT> calls;
        calls.reserve(chain.calls.size());
        for (size_t i = 0; i < chain.calls.size(); ++i) {
            // Only the top call may produce BSON or vary its input behavior and traversal depth.
            calls.push_back(makeCall(chain.calls[i], input, chain, i + 1 == chain.calls.size()));
        }
        // Keep calls separate for the rewrite. nestCalls() replaces each higher call's placeholder
        // input with the previous call's result when evaluating the original chain.
        return calls;
    }

    BSONObj evaluate(const abt::ABT& expr) {
        auto variables = abt::VariableEnvironment::build(expr);
        sbe::InputParamToSlotMap parameters;
        auto lowered =
            abt_lower::SBEExpressionLowering{
                variables, _slots, *_env.runtimeEnv, _slotIds, parameters}
                .optimize(expr);
        auto code = lowered->compileDirect(_env.ctx);
        sbe::vm::ByteCode vm;
        auto result = vm.run(&code);
        BSONObjBuilder bson;
        sbe::bson::appendValueToBsonObj(bson, "", result.tag(), result.value());
        return bson.obj();
    }

private:
    FieldAction convertAction(const ActionSpec& action, abt::ABTVector& args) {
        return std::visit(
            OverloadedVisitor{[](MakeObjSpec::Keep action) { return FieldAction{action}; },
                              [](MakeObjSpec::Drop action) { return FieldAction{action}; },
                              [&](const WriteSpec& write) {
                                  // Assign dense argument indexes as values are added. Keep and
                                  // Drop need none.
                                  const size_t argIdx = args.size();
                                  args.push_back(bindValue(makeValue(write.value), write.slot));
                                  return write.add ? FieldAction{MakeObjSpec::AddArg{argIdx}}
                                                   : FieldAction{MakeObjSpec::SetArg{argIdx}};
                              }},
            action);
    }

    CallFields convertFields(const CallSpec& call) {
        CallFields fields;
        // Preserve spec order because it controls the append order of SetArg and AddArg fields.
        for (int field : call.fieldOrder) {
            const auto& action = call.actions[field];
            if (!action) {
                // Unlisted fields use the scope's default action.
                continue;
            }
            fields.names.push_back(sbe::positionalName(field));
            fields.actions.push_back(convertAction(*action, fields.args));
        }
        return fields;
    }

    abt::ABT makeCall(const CallSpec& call,
                      const abt::ABT& input,
                      const ChainSpec& chain,
                      bool top) {
        auto fields = convertFields(call);
        // Lower calls always produce objects without traversing arrays.
        const auto depth = top ? chain.topDepth : std::optional<int32_t>{0};
        auto spec = std::make_unique<MakeObjSpec>(
            call.closed ? MakeObjSpec::FieldListScope::kClosed : MakeObjSpec::FieldListScope::kOpen,
            std::move(fields.names),
            std::move(fields.actions),
            top ? chain.topBehavior : MakeObjSpec::NonObjInputBehavior::kNewObj,
            depth ? boost::optional<int32_t>{*depth} : boost::none);

        // A call takes its constant spec, input, then the values referenced by argument indexes.
        auto nodes = abt::makeSeq(
            bindValue(TagValueOwned::fromRaw(TypeTags::makeObjSpec,
                                             sbe::value::bitcastFrom<MakeObjSpec*>(spec.release())),
                      /*slot=*/false),
            input);
        for (auto& arg : fields.args) {
            nodes.push_back(std::move(arg));
        }
        return abt::make<abt::FunctionCall>(
            top && chain.bsonTop ? sbe::EFn::kMakeBsonObj : sbe::EFn::kMakeObj, std::move(nodes));
    }

    abt::ABT bindValue(TagValueOwned value, bool slot) {
        auto [tag, val] = value.releaseToRaw();
        if (!slot) {
            return abt::make<abt::Constant>(tag, val);
        }
        const auto id = _env.runtimeEnv->registerSlot(tag, val, true, &_slotIds);
        auto name = SbVar::makeProjectionName(id);
        _slots.emplace(name, id);
        return abt::make<abt::Variable>(std::move(name));
    }

    Environment _env{std::make_unique<sbe::RuntimeEnvironment>()};
    sbe::value::SlotIdGenerator _slotIds;
    abt_lower::SlotVarMap _slots;
};

// This identity property exercises case generation and VM evaluation without a fusion dependency.
// Lower calls produce objects at depth zero. The top call varies its BSON conversion, behavior,
// and traversal depth. Field order, absent fields, and Nothing values are generated independently.
void MakeObjChainIdentity(const ChainSpec& spec) {
    MakeObjCase fixture;
    auto calls = fixture.makeCalls(spec);
    auto reference = oracle(calls);
    auto candidate = test(calls);
    auto expected = fixture.evaluate(reference);
    auto actual = fixture.evaluate(candidate);
    EXPECT_TRUE(expected.binaryEqual(actual))
        << "expected " << expected << ", got " << actual << "\n"
        << "oracle: " << abt::ExplainGenerator::explainV2(reference) << "\n"
        << "test: " << abt::ExplainGenerator::explainV2(candidate);
}

FUZZ_TEST(MakeObjFuzzTest, MakeObjChainIdentity).WithDomains(chainDomain());
}  // namespace
}  // namespace mongo::stage_builder
