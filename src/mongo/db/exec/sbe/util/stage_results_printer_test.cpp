/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/util/stage_results_printer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
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
