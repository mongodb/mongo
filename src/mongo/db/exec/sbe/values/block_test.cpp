/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

class SbeValueTest : public SbeStageBuilderTestFixture {};

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST_F(SbeValueTest, SbeValueBlockTypeIsCopyable) {
    value::MonoBlock block(1, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] = value::copyValue(value::TypeTags::valueBlock,
                                               value::bitcastFrom<value::MonoBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, value::TypeTags::valueBlock);
    auto cpy = value::getValueBlock(cpyValue);

    auto extracted = cpy->extract();
    ASSERT_EQ(extracted.count, 1);
}

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST_F(SbeValueTest, SbeCellBlockTypeIsCopyable) {
    value::ScalarMonoCellBlock block(
        1, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] = value::copyValue(
        value::TypeTags::cellBlock, value::bitcastFrom<value::ScalarMonoCellBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, value::TypeTags::cellBlock);
    auto cpy = value::getCellBlock(cpyValue);

    auto& vals = cpy->getValueBlock();
    auto extracted = vals.extract();
    ASSERT_EQ(extracted.count, 1);
}

// Other tests about CellBlocks, paths, additional ValueBlock functionality, can go here.

}  // namespace mongo::sbe
