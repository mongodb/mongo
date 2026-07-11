// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/slot_printer.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

namespace mongo::sbe::value {

TEST(SlotPrinterTest, SimpleMaterializedRow0) {
    MaterializedRow row;
    row.resize(0);

    std::ostringstream oss;
    SlotPrinters::make(oss, PrintOptions()).printMaterializedRow(row);

    ASSERT_EQ("[]", oss.str());
}

TEST(SlotPrinterTest, SimpleMaterializedRow1) {
    MaterializedRow row;
    row.resize(1);

    row.reset(0, false, TypeTags::NumberInt64, bitcastFrom<int64_t>(13ll));

    std::ostringstream oss;
    SlotPrinters::make(oss, PrintOptions()).printMaterializedRow(row);

    ASSERT_EQ("[13]", oss.str());
}

TEST(SlotPrinterTest, SimpleMaterializedRow2) {
    MaterializedRow row;
    row.resize(2);

    row.reset(0, false, TypeTags::NumberInt64, bitcastFrom<int64_t>(13ll));

    auto [tag1, val1] = makeNewString("abc");
    row.reset(1, true, tag1, val1);

    std::ostringstream oss;
    SlotPrinters::make(oss, PrintOptions()).printMaterializedRow(row);

    ASSERT_EQ("[13, \"abc\"]", oss.str());
}

}  // namespace mongo::sbe::value
