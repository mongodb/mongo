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

#include "mongo/db/exec/sbe/values/row.h"

#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/unittest/unittest.h"

#include <iterator>

namespace mongo::sbe {

static StringData longStrings[3] = {"long_string_1"_sd, "long_string_2"_sd, "long_string_3"_sd};
static size_t longStringsSize = std::end(longStrings) - std::begin(longStrings);

template <typename RowType, size_t N>
class RowTestBase {
public:
    void testBasic() {
        RowType row(N);
        ASSERT_EQ(row.size(), N);

        verifyAllNothing(row);
        setAllInt(row);
        verifyAllInt(row);
    }

    void testOwned() {
        RowType row(N);
        ASSERT_EQ(row.size(), N);

        verifyAllNothing(row);
        setAllLargeString(row);
        verifyAllLargeString(row);
    }

    void testUnowned() {
        RowType row(N);
        ASSERT_EQ(row.size(), N);

        verifyAllNothing(row);
        setAllLargeString(row);

        RowType row2(N);
        for (size_t i = 0; i < row.size(); i++) {
            setValue(row2, i, false, row.getViewOfValue(i));
        }
        verifyAllLargeString(row2);
    }

    void testCopy() {
        RowType row(N);
        ASSERT_EQ(row.size(), N);

        setAllLargeString(row);

        RowType row2(row);
        verifyAllLargeString(row);
        verifyAllLargeString(row2);
    }

    void testMove() {
        RowType row(N);
        ASSERT_EQ(row.size(), N);

        setAllLargeString(row);

        RowType row2(std::move(row));
        verifyAllNothing(row);
        verifyAllLargeString(row2);
    }

    void testAssign() {
        auto mkRow = [this]() {
            RowType row(N);
            setAllLargeString(row);
            return row;
        };

        RowType row(N);
        if (N > 0) {
            setValue(row, 0, true, value::makeNewString("other_long_string"_sd));
        }

        row = mkRow();
        ASSERT_EQ(row.size(), N);
        verifyAllLargeString(row);
    }

    void testCopyOrMoveValue() {
        RowType row(N);
        for (size_t i = 0; i < row.size(); i++) {
            auto expected = value::makeNewString(longStrings[i % longStringsSize]);
            value::ValueGuard expectedGuard(expected);

            setValue(row, i, true, value::makeNewString(longStrings[i % longStringsSize]));

            auto p1 = row.copyOrMoveValue(i);
            value::ValueGuard guard1(p1);
            ASSERT_THAT(p1, ValueEq(expected));
            verifyValue(row, i, copyValue(expected));

            setValue(row, i, false, expected);
            auto p2 = row.copyOrMoveValue(i);
            value::ValueGuard guard2(p2);
            ASSERT_THAT(p2, ValueEq(expected));
            verifyValue(row, i, copyValue(expected));
        }
    }

    void testResize() {
        RowType row(0);
        ASSERT_EQ(row.size(), 0);

        row.resize(N);
        ASSERT_EQ(row.size(), N);
        verifyAllNothing(row);
        setAllLargeString(row);
        verifyAllLargeString(row);


        row.resize(N / 2);
        ASSERT_EQ(row.size(), N / 2);
        verifyAllNothing(row);
        setAllLargeString(row);
        verifyAllLargeString(row);

        row.resize(N);
        verifyAllNothing(row);
        setAllLargeString(row);
        verifyAllLargeString(row);
    }

    void runTests() {
        testBasic();
        testOwned();
        testUnowned();
        testCopy();
        testMove();
        testAssign();
        testCopyOrMoveValue();

        if (allowResize) {
            testResize();
        }
    }

private:
    TypedValue makeInt(int value) {
        return std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value));
    }

    TypedValue copyValue(TypedValue p) {
        return value::copyValue(p.first, p.second);
    }

    void setValue(RowType& row, int idx, bool owned, TypedValue p) {
        row.reset(idx, owned, p.first, p.second);
    }

    void verifyValue(RowType& row, int idx, TypedValue p) {
        value::ValueGuard guard(p);
        ASSERT_THAT(row.getViewOfValue(idx), ValueEq(p));
    }

    void setAllNothing(RowType& row) {
        for (size_t i = 0; i < row.size(); i++) {
            setValue(row, i, false, std::make_pair(value::TypeTags::Nothing, 0));
        }
    }

    void verifyAllNothing(RowType& row) {
        for (size_t i = 0; i < row.size(); i++) {
            verifyValue(row, i, std::make_pair(value::TypeTags::Nothing, 0));
        }
    }

    void setAllInt(RowType& row) {
        for (size_t i = 0; i < row.size(); i++) {
            setValue(row, i, false, makeInt(i));
        }
    }

    void verifyAllInt(RowType& row) {
        for (size_t i = 0; i < N; i++) {
            verifyValue(row, i, makeInt(i));
        }
    }

    void setAllLargeString(RowType& row) {
        for (size_t i = 0; i < row.size(); i++) {
            setValue(row, i, true, value::makeNewString(longStrings[i % longStringsSize]));
        }
    }

    void verifyAllLargeString(RowType& row) {
        for (size_t i = 0; i < row.size(); i++) {
            verifyValue(row, i, value::makeNewString(longStrings[i % longStringsSize]));
        }
    }

public:
    bool allowResize{true};
};

class MaterializedRowTest0 : public RowTestBase<value::MaterializedRow, 0>,
                             public mongo::unittest::Test {};
TEST_F(MaterializedRowTest0, Tests) {
    runTests();
}

class MaterializedRowTest1 : public RowTestBase<value::MaterializedRow, 1>,
                             public mongo::unittest::Test {};
TEST_F(MaterializedRowTest1, Tests) {
    runTests();
}

class MaterializedRowTest3 : public RowTestBase<value::MaterializedRow, 3>,
                             public mongo::unittest::Test {};
TEST_F(MaterializedRowTest3, Tests) {
    runTests();
}

class FixedSizeRowTest1 : public RowTestBase<value::FixedSizeRow<1>, 1>,
                          public mongo::unittest::Test {};
TEST_F(FixedSizeRowTest1, Tests) {
    allowResize = false;
    runTests();
}

class FixedSizeRowTest3 : public RowTestBase<value::FixedSizeRow<3>, 3>,
                          public mongo::unittest::Test {};
TEST_F(FixedSizeRowTest3, Tests) {
    allowResize = false;
    runTests();
}

}  // namespace mongo::sbe
