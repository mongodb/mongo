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

#include <iterator>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

static StringData longString = {"long_string_1"_sd};

template <typename SlotType>
class SlotTestBase {
public:
    void testBasic() {
        SlotType slot;

        verifyNothing(slot);
        setInt(slot);
        verifyInt(slot);
    }

    void testOwned() {
        SlotType slot;

        verifyNothing(slot);
        setLargeString(slot);
        verifyLargeString(slot);
    }

    void testUnowned() {
        SlotType slot;

        verifyNothing(slot);
        setLargeString(slot);

        SlotType slot2;
        setValue(slot2, false, slot.getViewOfValue());

        verifyLargeString(slot2);
    }

    void testCopy() {
        SlotType slot;

        setLargeString(slot);

        SlotType slot2(slot);
        verifyLargeString(slot);
        verifyLargeString(slot);
    }

    void testMove() {
        SlotType slot;

        setLargeString(slot);

        SlotType slot2(std::move(slot));
        verifyLargeString(slot2);
    }

    void testAssign() {
        auto mkSlot = [this]() {
            SlotType slot;
            setLargeString(slot);
            return slot;
        };

        SlotType slot;
        setValue(slot, true, value::makeNewString("other_long_string"_sd));

        slot = mkSlot();
        verifyLargeString(slot);
    }

    void testCopyOrMoveValue() {
        SlotType slot;

        auto expected = value::makeNewString(longString);
        value::ValueGuard expectedGuard(expected);

        setValue(slot, true, value::makeNewString(longString));

        auto p1 = slot.copyOrMoveValue();
        value::ValueGuard guard1(p1);
        ASSERT_THAT(p1, ValueEq(expected));
        verifyValue(slot, copyValue(expected));

        setValue(slot, false, expected);
        auto p2 = slot.copyOrMoveValue();
        value::ValueGuard guard2(p2);
        ASSERT_THAT(p2, ValueEq(expected));
        verifyValue(slot, copyValue(expected));
    }

    void runTests() {
        testBasic();
        testOwned();
        testUnowned();
        testCopy();
        testMove();
        testAssign();
        testCopyOrMoveValue();
    }

private:
    TypedValue makeInt(int value) {
        return std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value));
    }

    TypedValue copyValue(TypedValue p) {
        return value::copyValue(p.first, p.second);
    }

    void setValue(SlotType& slot, bool owned, TypedValue p) {
        slot.reset(owned, p.first, p.second);
    }

    void verifyValue(SlotType& slot, TypedValue p) {
        value::ValueGuard guard(p);
        ASSERT_THAT(slot.getViewOfValue(), ValueEq(p));
    }

    void setNothing(SlotType& slot) {
        setValue(slot, false, std::make_pair(value::TypeTags::Nothing, 0));
    }

    void verifyNothing(SlotType& slot) {
        verifyValue(slot, std::make_pair(value::TypeTags::Nothing, 0));
    }

    void setInt(SlotType& slot) {
        setValue(slot, false, makeInt(1));
    }

    void verifyInt(SlotType& slot) {
        verifyValue(slot, makeInt(1));
    }

    void setLargeString(SlotType& slot) {
        setValue(slot, true, value::makeNewString(longString));
    }

    void verifyLargeString(SlotType& slot) {
        verifyValue(slot, value::makeNewString(longString));
    }


public:
    bool allowResize{true};
};

class OwnedValueAccessorTest : public SlotTestBase<value::OwnedValueAccessor>,
                               public mongo::unittest::Test {};
TEST_F(OwnedValueAccessorTest, Tests) {
    runTests();
}

class BSONObjValueAccessorTest : public SlotTestBase<value::BSONObjValueAccessor>,
                                 public mongo::unittest::Test {
public:
    void testGetOwnedBSONObj() {
        BSONObjBuilder bob;
        bob.append("a", 1);
        BSONObj obj = bob.obj();

        // Test getOwnedBSONObj on unowned bsonObject
        {
            value::BSONObjValueAccessor slot;
            slot.reset(
                false, value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata()));
            auto [tag, val] = slot.getViewOfValue();
            auto slotData = value::bitcastTo<const char*>(val);

            BSONObj obj2 = slot.getOwnedBSONObj();
            ASSERT_TRUE(strcmp(obj2.objdata(), obj.objdata()) == 0);
            ASSERT_THAT(obj2.objdata(), Ne(slotData));

            BSONObj obj3 = slot.getOwnedBSONObj();
            ASSERT_TRUE(strcmp(obj3.objdata(), obj.objdata()) == 0);
            ASSERT_THAT(obj2.objdata(), Ne(slotData));
            ASSERT_THAT(obj3.objdata(), Eq(obj2.objdata()));
        }

        // Test getOwnedBSONObj on owned bsonObject
        {
            value::BSONObjValueAccessor slot;
            slot.reset(
                false, value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata()));
            slot.makeOwned();
            auto [tag, val] = slot.getViewOfValue();
            auto slotData = value::bitcastTo<const char*>(val);

            BSONObj obj2 = slot.getOwnedBSONObj();
            ASSERT_TRUE(strcmp(obj2.objdata(), obj.objdata()) == 0);
            ASSERT_THAT(obj2.objdata(), Eq(slotData));

            slot.makeOwned();
            auto [tag2, val2] = slot.getViewOfValue();
            ASSERT_THAT(val2, Eq(val));
        }
    }

    void runTests() {
        SlotTestBase::runTests();
        testGetOwnedBSONObj();
    }
};
TEST_F(BSONObjValueAccessorTest, Tests) {
    runTests();
}

}  // namespace mongo::sbe
