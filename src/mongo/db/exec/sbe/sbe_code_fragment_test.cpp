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

#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class SBECodeFragmentTest : public GoldenSBETestFixture {
public:
    SBECodeFragmentTest() : GoldenSBETestFixture() {}

    void runTest(const vm::CodeFragment& code) {
        auto& os = gctx->outStream();
        os << "-- CODE:" << std::endl;
        makeCodeFragmentPrinter().print(os, code);
        os << std::endl;

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);
        value::ValueGuard guard(owned, tag, val);

        os << "-- RESULT:" << std::endl;
        makeValuePrinter(os).writeValueToStream(tag, val);
        os << std::endl << std::endl;
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
        code.appendSub({0, frameId}, {});
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
        code2.appendSub({0, frameId}, {});

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
        instr.appendSub({0, frameId}, {});

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
        instr.appendSub({0, frameId}, {});

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
        code.appendSub({}, {0, frameId});
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
        code2.appendSub({}, {0, frameId});

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
        instr.appendSub({}, {0, frameId});

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
        instr.appendSub({}, {0, frameId});

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

        code.appendSub({0, frameId}, {1, frameId});
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
        code2.appendSub({0, frameId}, {1, frameId});

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
        instr.appendSub({0, frameId}, {1, frameId});

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

}  // namespace mongo::sbe
