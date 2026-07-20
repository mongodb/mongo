// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/unittest/unittest.h"

#include <string_view>


namespace mongo::sbe {
using namespace std::literals::string_view_literals;

static std::string_view longString = {"long_string_1"sv};

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
        setValue(slot, true, value::makeNewString("other_long_string"sv));

        slot = mkSlot();
        verifyLargeString(slot);
    }

    void testCopyOrMoveValue() {
        SlotType slot;

        auto expected = value::makeNewString(longString);
        value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(expected);

        setValue(slot, true, value::makeNewString(longString));

        auto p1 = slot.copyOrMoveValue();
        ASSERT_THAT(p1.raw(), ValueEq(expected));
        verifyValue(slot, copyValue(expected));

        setValue(slot, false, expected);
        auto p2 = slot.copyOrMoveValue();
        ASSERT_THAT(p2.raw(), ValueEq(expected));
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
        slot.reset(value::TagValueMaybeOwned{owned, p.first, p.second});
    }

    void verifyValue(SlotType& slot, TypedValue p) {
        value::TagValueOwned pOwned = value::TagValueOwned::fromRaw(p);
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
        using testing::Eq;
        using testing::Ne;
        BSONObjBuilder bob;
        bob.append("a", 1);
        BSONObj obj = bob.obj();

        // Test getOwnedBSONObj on unowned bsonObject
        {
            value::BSONObjValueAccessor slot;
            slot.reset(value::TagValueView{value::TypeTags::bsonObject,
                                           value::bitcastFrom<const char*>(obj.objdata())});
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
            slot.reset(value::TagValueView{value::TypeTags::bsonObject,
                                           value::bitcastFrom<const char*>(obj.objdata())});
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
