/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>

namespace mongo::sbe {

class SBECodeFragmentTest : public GoldenSBETestFixture {
public:
    SBECodeFragmentTest() : GoldenSBETestFixture() {}

    // Pretty prints 'code', executes it, and pretty prints the result.
    void runTest(const vm::CodeFragment& code) {
        std::ostream& os = gctx->outStream();

        printCodeFragment(code);

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);
        value::ValueGuard guard(owned, tag, val);

        os << "-- RESULT:" << std::endl;
        makeValuePrinter(os).writeValueToStream(tag, val);
        os << std::endl << std::endl;
    }

    // Pretty prints 'code' without attempting to execute it.
    void printCodeFragment(const vm::CodeFragment& code) {
        std::ostream& os = gctx->outStream();

        os << "-- CODE:" << std::endl;
        makeCodeFragmentPrinter().print(os, code);
        os << std::endl;
    }

    std::pair<value::TypeTags, value::Value> makeInt32(int value) {
        return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value)};
    }
};

TEST_F(SBECodeFragmentTest, AppendSimpleInstruction_Binary_BothOnStack) {
    auto lhsValue = makeInt32(10);
    auto rhsValue = makeInt32(20);

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendSub({}, {});

        runTest(code);
    }

    {
        printVariation("append code");

        vm::CodeFragment lhs;
        lhs.appendConstVal(lhsValue.first, lhsValue.second);
        vm::CodeFragment rhs;
        rhs.appendConstVal(rhsValue.first, rhsValue.second);

        vm::CodeFragment instr;
        instr.appendSub({}, {});

        vm::CodeFragment code;
        code.append(std::move(lhs));
        code.append(std::move(rhs));
        code.append(std::move(instr));

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, AppendSimpleInstruction_Binary_LhsOnFrame) {
    auto lhsValue = makeInt32(10);
    auto rhsValue = makeInt32(20);
    FrameId frameId = 10;

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendSub({0, 0, frameId}, {});
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 1");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendConstVal(rhsValue.first, rhsValue.second);
        code2.appendSub({0, 0, frameId}, {});

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 2");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment rhs;
        rhs.appendConstVal(rhsValue.first, rhsValue.second);

        vm::CodeFragment instr;
        instr.appendSub({0, 0, frameId}, {});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(rhs));
        code.append(std::move(instr));

        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 3");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment rhs;
        rhs.appendConstVal(rhsValue.first, rhsValue.second);

        vm::CodeFragment instr;
        instr.append(std::move(rhs));
        instr.appendSub({0, 0, frameId}, {});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(instr));

        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, AppendSimpleInstruction_Binary_RhsOnFrame) {
    auto lhsValue = makeInt32(10);
    auto rhsValue = makeInt32(20);
    FrameId frameId = 10;

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendSub({}, {0, 0, frameId});
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 1");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendConstVal(lhsValue.first, lhsValue.second);
        code2.appendSub({}, {0, 0, frameId});

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 2");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment lhs;
        lhs.appendConstVal(lhsValue.first, lhsValue.second);

        vm::CodeFragment instr;
        instr.appendSub({}, {0, 0, frameId});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(lhs));
        code.append(std::move(instr));

        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 3");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment lhs;
        lhs.appendConstVal(lhsValue.first, lhsValue.second);

        vm::CodeFragment instr;
        instr.append(std::move(lhs));
        instr.appendSub({}, {0, 0, frameId});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(instr));

        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}


TEST_F(SBECodeFragmentTest, AppendSimpleInstruction_Binary_BothOnFrame) {
    auto lhsValue = makeInt32(10);
    auto rhsValue = makeInt32(20);
    FrameId frameId = 10;

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendSub({0, 0, frameId}, {1, 0, frameId});
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 1");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendSub({0, 0, frameId}, {1, 0, frameId});

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 2");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment instr;
        instr.appendSub({0, 0, frameId}, {1, 0, frameId});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(instr));

        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, AppendLocalVal) {
    auto value = makeInt32(10);
    FrameId frameId = 10;

    {
        printVariation("append code");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(value.first, value.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 0, false);

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(value.first, value.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendLocalVal(frameId, 0, false);
        code.removeFrame(frameId);

        for (int i = 0; i < 2; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

// Tests vm::CodeFragmentPrinter's ability to print fixups in unresolved CodeFragments.
TEST_F(SBECodeFragmentTest, AppendLocalValWithFixups) {
    FrameId frameId = 10;

    {
        printVariation("append local val with fixups");

        vm::CodeFragment code;
        code.declareFrame(frameId);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 10, false);
        code2.appendLocalVal(frameId, 20, false);
        code2.appendLocalVal(frameId, 0, false);
        code2.appendLocalVal(frameId, 0, false);
        code2.appendLocalVal(frameId, 100, false);

        code.removeFrame(frameId);

        printCodeFragment(code2);
    }
}

TEST_F(SBECodeFragmentTest, AppendLocalVal2) {
    auto lhsValue = makeInt32(10);
    auto rhsValue = makeInt32(20);
    FrameId frameId = 10;

    {
        printVariation("append code 1");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 0, false);
        code2.appendLocalVal(frameId, 1, false);
        code2.appendSub({}, {});

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 2");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 0, false);
        code2.appendLocalVal(frameId, 1, false);
        code2.appendSub({}, {});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 3");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment lhs;
        lhs.appendLocalVal(frameId, 0, false);

        vm::CodeFragment rhs;
        rhs.appendLocalVal(frameId, 1, false);

        vm::CodeFragment instr;
        instr.appendSub({}, {});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(lhs));
        code.append(std::move(rhs));
        code.append(std::move(instr));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 4");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment lhs;
        lhs.appendLocalVal(frameId, 0, false);

        vm::CodeFragment rhs;
        rhs.appendLocalVal(frameId, 1, false);

        vm::CodeFragment code2;
        code2.append(std::move(lhs));
        code2.append(std::move(rhs));
        code2.appendSub({}, {});

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append code 5");

        vm::CodeFragment frame;
        frame.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        frame.appendConstVal(lhsValue.first, lhsValue.second);
        frame.appendConstVal(rhsValue.first, rhsValue.second);
        frame.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment lhs;
        lhs.appendLocalVal(frameId, 0, false);

        vm::CodeFragment rhs;
        rhs.appendLocalVal(frameId, 1, false);

        vm::CodeFragment instr;
        instr.appendSub({}, {});

        vm::CodeFragment code2;
        code2.append(std::move(lhs));
        code2.append(std::move(rhs));
        code2.append(std::move(instr));

        vm::CodeFragment code;
        code.append(std::move(frame));
        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendLocalVal(frameId, 0, false);
        code.appendLocalVal(frameId, 1, false);
        code.appendSub({}, {});
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, AppendMakeOwn) {
    auto lhsValue = value::makeBigString("one not too short string");
    value::ValueGuard lhsGuard(lhsValue);
    auto rhsValue = value::makeBigString("another string");
    value::ValueGuard rhsGuard(rhsValue);
    FrameId frameId = 10;

    {
        printVariation("append code 1");

        vm::CodeFragment code;
        code.declareFrame(frameId);
        // Create frame with 3 variables to make variable resolution less trivial.
        code.appendConstVal(lhsValue.first, lhsValue.second);
        code.appendConstVal(rhsValue.first, rhsValue.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 0, false);
        code2.appendMakeOwn({});
        code2.appendLocalVal(frameId, 1, false);
        code2.appendMakeOwn({});
        code2.appendFunction(vm::Builtin::concat, 2);

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 3; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, DeclareFrameNotEmptyStack) {
    auto value = makeInt32(10);
    FrameId frameId = 10;

    {
        printVariation("append code");

        vm::CodeFragment code;
        // Pad the stack with 3 values
        code.appendConstVal(value::TypeTags::Nothing, 0);
        code.appendConstVal(value::TypeTags::Nothing, 0);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(value.first, value.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        vm::CodeFragment code2;
        code2.appendLocalVal(frameId, 0, false);

        code.append(std::move(code2));
        code.removeFrame(frameId);

        for (int i = 0; i < 5; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }

    {
        printVariation("append instr");

        vm::CodeFragment code;
        // Pad the stack with 3 values
        code.appendConstVal(value::TypeTags::Nothing, 0);
        code.appendConstVal(value::TypeTags::Nothing, 0);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.declareFrame(frameId);
        // Create frame with 2 variables to make variable resolution less trivial.
        code.appendConstVal(value.first, value.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);

        code.appendLocalVal(frameId, 0, false);
        code.removeFrame(frameId);

        for (int i = 0; i < 5; i++) {
            code.appendSwap();
            code.appendPop();
        }

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, LabelJump) {
    auto value1 = makeInt32(10);
    auto value2 = makeInt32(20);
    vm::LabelId label1 = 100;
    vm::LabelId label2 = 200;
    vm::LabelId label3 = 300;

    {
        printVariation("append instr");

        vm::CodeFragment code;
        code.appendConstVal(value1.first, value1.second);
        code.appendLabelJump(label1);
        code.appendLabel(label2);
        code.appendSub({}, {});
        code.appendLabelJump(label3);
        code.appendLabel(label1);
        code.appendConstVal(value2.first, value2.second);
        code.appendLabelJump(label2);
        code.appendLabel(label3);

        code.removeLabel(label1);
        code.removeLabel(label2);
        code.removeLabel(label3);

        runTest(code);
    }

    {
        printVariation("append code");

        vm::CodeFragment code1;
        code1.appendConstVal(value1.first, value1.second);
        code1.appendLabelJump(label1);
        code1.appendLabel(label2);

        vm::CodeFragment code2;
        code2.appendSub({}, {});
        code2.appendLabelJump(label3);
        code2.appendLabel(label1);

        vm::CodeFragment code3;
        code3.appendConstVal(value2.first, value2.second);
        code3.appendLabelJump(label2);
        code3.appendLabel(label3);

        vm::CodeFragment code;
        code2.append(std::move(code3));
        code.append(std::move(code1));
        code.append(std::move(code2));

        code.removeLabel(label1);
        code.removeLabel(label2);
        code.removeLabel(label3);


        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, LabelJumpTrue) {
    auto value1 = makeInt32(10);
    auto value2 = makeInt32(20);
    vm::LabelId label1 = 100;
    vm::LabelId label2 = 200;

    {
        printVariation("basic sanity check");

        vm::CodeFragment code1;
        code1.appendConstVal(value1.first, value1.second);
        code1.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        code1.appendLabelJumpTrue(label2);

        vm::CodeFragment code2;
        code2.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        code2.appendLabelJumpTrue(label1);
        code2.appendLabelJump(label2);

        vm::CodeFragment code3;
        code3.appendLabel(label1);
        code3.appendConstVal(value2.first, value2.second);
        code3.appendSub({}, {});
        code1.appendLabel(label2);

        vm::CodeFragment code;
        code2.append(std::move(code3));
        code.append(std::move(code1));
        code.append(std::move(code2));

        code.removeLabel(label1);
        code.removeLabel(label2);

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, LabelJumpFalse) {
    auto value1 = makeInt32(10);
    auto value2 = makeInt32(20);
    vm::LabelId label1 = 100;
    vm::LabelId label2 = 200;

    {
        printVariation("basic sanity check");

        vm::CodeFragment code1;
        code1.appendConstVal(value1.first, value1.second);
        code1.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        code1.appendLabelJumpFalse(label2);

        vm::CodeFragment code2;
        code2.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        code2.appendLabelJumpFalse(label1);
        code2.appendLabelJump(label2);

        vm::CodeFragment code3;
        code3.appendLabel(label1);
        code3.appendConstVal(value2.first, value2.second);
        code3.appendSub({}, {});
        code1.appendLabel(label2);

        vm::CodeFragment code;
        code2.append(std::move(code3));
        code.append(std::move(code1));
        code.append(std::move(code2));

        code.removeLabel(label1);
        code.removeLabel(label2);

        runTest(code);
    }
}

TEST_F(SBECodeFragmentTest, LabelJumpNothing) {
    auto value1 = makeInt32(10);
    auto value2 = makeInt32(20);
    vm::LabelId labelThen1 = 100;
    vm::LabelId labelThen2 = 200;
    vm::LabelId labelEnd = 300;

    {
        printVariation("basic sanity check");

        vm::CodeFragment code;
        code.appendConstVal(value1.first, value1.second);
        code.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        code.appendLabelJumpNothing(labelThen1);

        vm::CodeFragment else1;
        else1.appendPop();
        else1.appendConstVal(value::TypeTags::Nothing, 0);
        else1.appendLabelJumpNothing(labelThen2);

        vm::CodeFragment else2;
        else2.appendPop();
        else2.appendLabelJump(labelEnd);

        vm::CodeFragment then2;
        then2.appendLabel(labelThen2);
        then2.appendPop();
        then2.appendConstVal(value2.first, value2.second);
        then2.appendSub({}, {});

        else1.append({std::move(else2), std::move(then2)});
        else1.appendLabelJump(labelEnd);

        vm::CodeFragment then1;
        then1.appendLabel(labelThen1);
        then1.appendPop();

        code.append({std::move(else1), std::move(then1)});
        code.appendLabel(labelEnd);

        code.removeLabel(labelThen1);
        code.removeLabel(labelThen2);
        code.removeLabel(labelEnd);

        runTest(code);
    }
}


TEST_F(SBECodeFragmentTest, LabelJumpNotNothing) {
    auto value1 = makeInt32(10);
    auto value2 = makeInt32(20);
    vm::LabelId labelThen1 = 100;
    vm::LabelId labelThen2 = 200;
    vm::LabelId labelEnd = 300;

    {
        printVariation("basic sanity check");

        vm::CodeFragment code;
        code.appendConstVal(value1.first, value1.second);
        code.appendConstVal(value::TypeTags::Nothing, 0);
        code.appendLabelJumpNotNothing(labelThen1);

        vm::CodeFragment else1;
        else1.appendPop();
        else1.appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        else1.appendLabelJumpNotNothing(labelThen2);

        vm::CodeFragment else2;
        else2.appendPop();
        else2.appendLabelJump(labelEnd);

        vm::CodeFragment then2;
        then2.appendLabel(labelThen2);
        then2.appendPop();
        then2.appendConstVal(value2.first, value2.second);
        then2.appendSub({}, {});

        else1.append({std::move(else2), std::move(then2)});
        else1.appendLabelJump(labelEnd);

        vm::CodeFragment then1;
        then1.appendLabel(labelThen1);
        then1.appendPop();

        code.append({std::move(else1), std::move(then1)});
        code.appendLabel(labelEnd);

        code.removeLabel(labelThen1);
        code.removeLabel(labelThen2);
        code.removeLabel(labelEnd);

        runTest(code);
    }
}


}  // namespace mongo::sbe
