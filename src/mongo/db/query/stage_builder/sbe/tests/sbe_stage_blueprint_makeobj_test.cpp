// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_makeobj.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <functional>
#include <string>
#include <vector>

namespace mongo::stage_builder {
namespace {
using sbe::MakeObjSpec;
using FieldAction = MakeObjSpec::FieldAction;
using Scope = MakeObjSpec::FieldListScope;
using NonObj = MakeObjSpec::NonObjInputBehavior;
using sbe::value::TypeTags;

// Returns 'as' as a vector of actions. FieldAction is move-only, so it can't be listed in an
// initializer list.
template <typename... As>
std::vector<FieldAction> acts(As&&... as) {
    std::vector<FieldAction> v;
    (v.push_back(FieldAction{std::forward<As>(as)}), ...);
    return v;
}

std::unique_ptr<MakeObjSpec> makeSpec(Scope scope,
                                      std::vector<std::string> fields,
                                      std::vector<FieldAction> actions,
                                      NonObj nonObj = NonObj::kNewObj,
                                      boost::optional<int32_t> traversalDepth = 0) {
    return std::make_unique<MakeObjSpec>(
        scope, std::move(fields), std::move(actions), nonObj, traversalDepth);
}

bool isSlotVariable(const abt::ABT& expr) {
    const auto* var = expr.cast<abt::Variable>();
    return var && SbSlot::fromProjectionName(var->name());
}

// Whether reading 'expr' costs no more than the getField() it replaces: a slot, a constant or a
// field of a slot.
bool isExprCheapToRepeat(const abt::ABT& expr) {
    if (isSlotVariable(expr) || expr.is<abt::Constant>()) {
        return true;
    }
    const auto* call = expr.cast<abt::FunctionCall>();
    return call && call->fn() == sbe::EFn::kGetField && isSlotVariable(call->nodes()[0]) &&
        call->nodes()[1].is<abt::Constant>();
}

// Returns 'v' as the only field of a BSON object, or an empty object if 'v' is Nothing.
BSONObj toBson(const sbe::value::TagValueMaybeOwned& v) {
    BSONObjBuilder bob;
    sbe::bson::appendValueToBsonObj(bob, "", v.tag(), v.value());
    return bob.obj();
}

/**
 * Each case builds a call over slots holding real values and says whether the helper should read
 * 'field' through it.
 *
 * When the helper reads the field, the test shows that the expression it returns is equivalent to
 * getField(<call>, field) by running both. The call's input and arguments are slots registered in
 * the runtime environment with concrete values, so both expressions read exactly the same inputs.
 * The test lowers each expression to an EExpression, compiles it and runs it in the VM, then
 * requires the two results to be the same MQL value: converted to BSON, they must be identical
 * byte for byte. That ignores how SBE represents a value, such as an SBE array versus a BSON
 * array, but not its BSON type or, for an object, the order of its fields.
 */
class SbBlueprintReadFieldThroughMakeObjTest : public GoldenSbeExprBuilderTestFixture {
protected:
    // Whether the helper should read the field through the call.
    enum class Expect { kReads, kDeclines };

    struct Case {
        std::string name;
        std::function<SbExpr()> call;
        std::string field;
        // What the helper is told about the call's input.
        TypeSignature inputType;
        Expect expect;
    };

    SbExprBuilder b() {
        return SbExprBuilder{*_state};
    }

    // Returns a variable reading a new slot that holds 'value'.
    SbExpr slot(sbe::value::TagValueOwned value) {
        auto [tag, val] = value.releaseToRaw();
        return b().makeVariable(_env->registerSlot(tag, val, /*owned=*/true, &_slotIdGenerator));
    }

    static sbe::value::TagValueOwned arraySetOf(int32_t n) {
        auto set = sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewArraySet());
        sbe::value::getArraySetView(set.value())
            ->push_back(sbe::value::TagValueOwned::numberInt32(n));
        return set;
    }

    SbExpr slot(const BSONObj& obj) {
        return slot(sbe::value::TagValueOwned::fromRaw(makeValue(obj)));
    }

    SbExpr slot(const BSONArray& arr) {
        return slot(sbe::value::TagValueOwned::fromRaw(makeValue(arr)));
    }

    SbExpr slot(int32_t n) {
        return slot(sbe::value::TagValueOwned::numberInt32(n));
    }

    SbExpr spec(std::unique_ptr<MakeObjSpec> s) {
        return b().makeConstant(sbe::value::TagValueOwned::fromRaw(
            TypeTags::makeObjSpec, sbe::value::bitcastFrom<MakeObjSpec*>(s.release())));
    }

    SbExpr getField(SbExpr obj, std::string_view field) {
        return b().makeFunction(sbe::EFn::kGetField, std::move(obj), b().makeStrConstant(field));
    }

    sbe::value::TagValueMaybeOwned evaluate(SbExpr expr) {
        auto eexpr = expr.lower(*_state);
        sbe::vm::CodeFragment code = eexpr->compileDirect(_env.ctx);
        sbe::vm::ByteCode vm;
        return vm.run(&code);
    }

    void run(std::vector<Case> cases) {
        for (auto& c : cases) {
            auto call = c.call().extractABT();
            auto read = readFieldThroughMakeObj(call, c.field, c.inputType);
            ASSERT_EQ(read.has_value(), c.expect == Expect::kReads) << c.name;
            if (!read) {
                continue;
            }
            ASSERT_TRUE(isExprCheapToRepeat(*read)) << c.name;

            auto expected = toBson(evaluate(getField(SbExpr{abt::ABT{call}}, c.field)));
            auto actual = toBson(evaluate(SbExpr{std::move(*read)}));
            ASSERT_TRUE(expected.binaryEqual(actual))
                << c.name << ": expected " << expected << ", got " << actual;
        }
    }

    SbExpr makeObj(std::unique_ptr<MakeObjSpec> s, SbExpr input) {
        return b().makeFunction(sbe::EFn::kMakeObj, spec(std::move(s)), std::move(input));
    }

    SbExpr makeObj(std::unique_ptr<MakeObjSpec> s, SbExpr input, SbExpr arg) {
        return b().makeFunction(
            sbe::EFn::kMakeObj, spec(std::move(s)), std::move(input), std::move(arg));
    }

    const BSONObj doc = BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2));
    const TypeSignature object = TypeSignature::kObjectType;
    const TypeSignature unknown = TypeSignature::kAnyType;
};

TEST_F(SbBlueprintReadFieldThroughMakeObjTest, KeptAndDroppedFields) {
    auto keepA = [] {
        return makeSpec(Scope::kOpen, {"a"}, acts(MakeObjSpec::Keep{}));
    };
    run({
        {"kept field", [&] { return makeObj(keepA(), slot(doc)); }, "a", object, Expect::kReads},
        {"unlisted field of an open spec",
         [&] { return makeObj(keepA(), slot(doc)); },
         "b",
         object,
         Expect::kReads},
        {"kept field the input lacks",
         [&] { return makeObj(keepA(), slot(doc)); },
         "z",
         object,
         Expect::kReads},
        {"dropped field",
         [&] {
             return makeObj(makeSpec(Scope::kOpen, {"a"}, acts(MakeObjSpec::Drop{})), slot(doc));
         },
         "a",
         object,
         Expect::kReads},
        {"unlisted field of a closed spec",
         [&] {
             return makeObj(makeSpec(Scope::kClosed, {"a"}, acts(MakeObjSpec::Keep{})), slot(doc));
         },
         "b",
         object,
         Expect::kReads},
        // Reading the field from the input would evaluate the input again.
        {"kept field of an input that isn't a slot",
         [&] { return makeObj(keepA(), getField(slot(BSON("sub" << doc)), "sub")); },
         "a",
         object,
         Expect::kDeclines},
    });
}

TEST_F(SbBlueprintReadFieldThroughMakeObjTest, SetAndAddedFields) {
    auto setC = [](NonObj nonObj = NonObj::kNewObj, boost::optional<int32_t> depth = 0) {
        return makeSpec(Scope::kOpen, {"c"}, acts(MakeObjSpec::SetArg{0}), nonObj, depth);
    };
    run({
        {"set field",
         [&] { return makeObj(setC(), slot(doc), slot(7)); },
         "c",
         object,
         Expect::kReads},
        {"added field",
         [&] {
             return makeObj(makeSpec(Scope::kClosed, {"c"}, acts(MakeObjSpec::AddArg{0})),
                            slot(doc),
                            b().makeInt32Constant(7));
         },
         "c",
         object,
         Expect::kReads},
        // What a non-object input becomes depends on the spec, so the input must be known to be an
        // object.
        {"set field over a non-object input",
         [&] { return makeObj(setC(), slot(5), slot(7)); },
         "c",
         unknown,
         Expect::kDeclines},
        {"non-object input that makes the result Nothing",
         [&] { return makeObj(setC(NonObj::kReturnNothing), slot(5), slot(7)); },
         "c",
         unknown,
         Expect::kDeclines},
        {"object input to a spec that returns Nothing otherwise",
         [&] { return makeObj(setC(NonObj::kReturnNothing), slot(doc), slot(7)); },
         "c",
         object,
         Expect::kReads},
        // A spec that traverses arrays turns an array input into an array of objects.
        {"traversing spec over an input that may be an array",
         [&] {
             return makeObj(setC(NonObj::kNewObj, boost::none), slot(BSON_ARRAY(doc)), slot(7));
         },
         "c",
         unknown,
         Expect::kDeclines},
        {"traversing spec over an object input",
         [&] { return makeObj(setC(NonObj::kNewObj, boost::none), slot(doc), slot(7)); },
         "c",
         object,
         Expect::kReads},
        // Reading the argument would evaluate it again.
        {"argument that isn't a slot or a constant",
         [&] { return makeObj(setC(), slot(doc), getField(slot(doc), "a")); },
         "c",
         object,
         Expect::kDeclines},
    });
}

TEST_F(SbBlueprintReadFieldThroughMakeObjTest, SetValues) {
    // makeObj() stores values unchanged, so a set stays a set.
    run({
        {"set-valued argument",
         [&] {
             return makeObj(makeSpec(Scope::kOpen, {"c"}, acts(MakeObjSpec::SetArg{0})),
                            slot(doc),
                            slot(arraySetOf(1)));
         },
         "c",
         object,
         Expect::kReads},
        {"kept set field of an SBE object",
         [&] {
             auto obj = sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewObject());
             sbe::value::getObjectView(obj.value())->push_back("a", arraySetOf(1));
             return makeObj(makeSpec(Scope::kOpen, {"a"}, acts(MakeObjSpec::Keep{})),
                            slot(std::move(obj)));
         },
         "a",
         object,
         Expect::kReads},
    });
}

// The helper gives up on anything it would have to evaluate or interpret itself to read the field:
// LambdaArg actions, which run a lambda over the input's field; nested MakeObj actions, which build
// a sub-object from it; makeBsonObj() calls, which convert what they store to BSON; and calls that
// aren't makeObj() at all. A rewrite then leaves getField() of the call as it is.
TEST_F(SbBlueprintReadFieldThroughMakeObjTest, CallsThatAreNotRead) {
    run({
        // makeBsonObj() converts what it stores to BSON, which turns a set into an array.
        {"makeBsonObj() call",
         [&] {
             return b().makeFunction(
                 sbe::EFn::kMakeBsonObj,
                 spec(makeSpec(Scope::kOpen, {"c"}, acts(MakeObjSpec::SetArg{0}))),
                 slot(doc),
                 slot(arraySetOf(1)));
         },
         "c",
         object,
         Expect::kDeclines},
        {"lambda field",
         [&] {
             auto frameId = _state->frameId();
             return makeObj(makeSpec(Scope::kOpen, {"a"}, acts(MakeObjSpec::LambdaArg{0, false})),
                            slot(doc),
                            b().makeLocalLambda(frameId, b().makeVariable(SbVar{frameId, 0})));
         },
         "a",
         object,
         Expect::kDeclines},
        {"nested object field",
         [&] {
             return makeObj(
                 makeSpec(Scope::kOpen,
                          {"a"},
                          acts(makeSpec(Scope::kClosed, {"b"}, acts(MakeObjSpec::Keep{})))),
                 slot(doc));
         },
         "a",
         object,
         Expect::kDeclines},
        {"not a makeObj() call",
         [&] { return getField(slot(doc), "a"); },
         "a",
         object,
         Expect::kDeclines},
        {"newObj() call",
         [&] { return b().makeFunction(sbe::EFn::kNewObj, b().makeStrConstant("a"), slot(7)); },
         "a",
         object,
         Expect::kDeclines},
    });
}

using SbBlueprintReadFieldThroughMakeObjDeathTest = SbBlueprintReadFieldThroughMakeObjTest;

DEATH_TEST_F(SbBlueprintReadFieldThroughMakeObjDeathTest,
             CallMissingAnArgumentItsSpecRefersTo,
             "13888800") {
    auto call = makeObj(makeSpec(Scope::kOpen, {"c"}, acts(MakeObjSpec::SetArg{0})), slot(doc))
                    .extractABT();
    readFieldThroughMakeObj(call, "c", object);
}
}  // namespace
}  // namespace mongo::stage_builder
