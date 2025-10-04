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

#include "mongo/db/exec/sbe/values/slot_printer.h"

#include "mongo/base/string_data.h"
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
