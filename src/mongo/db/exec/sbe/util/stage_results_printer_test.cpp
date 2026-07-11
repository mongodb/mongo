// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/stage_results_printer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::sbe {

class StageResultsPrinterTestFixture : public PlanStageTestFixture {
public:
    void runTest(const std::vector<std::string>& names,
                 const BSONArray& input,
                 const PrintOptions& options,
                 const std::string expected) {
        auto ctx = makeCompileCtx();
        auto [scanSlots, scanStage] = generateVirtualScanMulti(names.size(), input);

        prepareTree(ctx.get(), scanStage.get());

        std::stringstream stream;
        StageResultsPrinters::make(stream, options)
            .printStageResults(ctx.get(), scanSlots, names, scanStage.get());

        ASSERT_EQ(stream.str(), expected);

        scanStage->close();
    }
};

TEST_F(StageResultsPrinterTestFixture, SimpleStage) {
    runTest({"a", "b"},
            BSON_ARRAY(BSON_ARRAY(1 << "abc") << BSON_ARRAY(2.5 << "def")),
            PrintOptions(),
            "[a, b]:\n"
            "1, \"abc\"\n"
            "2.5, \"def\"\n");
}

TEST_F(StageResultsPrinterTestFixture, SimpleStageWithLimit) {
    runTest({"a", "b"},
            BSON_ARRAY(BSON_ARRAY(1 << "abc") << BSON_ARRAY(2.5 << "def")),
            PrintOptions().arrayObjectOrNestingMaxDepth(1),
            "[a, b]:\n"
            "1, \"abc\"\n"
            "...\n");
}
}  // namespace mongo::sbe
