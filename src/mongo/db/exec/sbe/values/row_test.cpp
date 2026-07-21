// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/row.h"

#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/unittest/unittest.h"

#include <iterator>
#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

static std::string_view longStrings[3] = {"long_string_1"sv, "long_string_2"sv, "long_string_3"sv};
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
            setValue(row, 0, true, value::makeNewString("other_long_string"sv));
        }

        row = mkRow();
        ASSERT_EQ(row.size(), N);
        verifyAllLargeString(row);
    }

    void testCopyOrMoveValue() {
        RowType row(N);
        for (size_t i = 0; i < row.size(); i++) {
            auto expected = value::makeNewString(longStrings[i % longStringsSize]);
            value::TagValueOwned expectedOwner = value::TagValueOwned::fromRaw(expected);

            setValue(row, i, true, value::makeNewString(longStrings[i % longStringsSize]));

            auto p1 = row.copyOrMoveValue(i);
            ASSERT_THAT(p1.raw(), ValueEq(expected));
            verifyValue(row, i, copyValue(expected));

            setValue(row, i, false, expected);
            auto p2 = row.copyOrMoveValue(i);
            ASSERT_THAT(p2.raw(), ValueEq(expected));
            verifyValue(row, i, copyValue(expected));
        }
    }

    void testResetView() {
        RowType row(N);
        for (size_t i = 0; i < row.size(); i++) {
            // Own the string here; hand the row only a non-owning *view* of it.
            auto owned = value::makeNewString(longStrings[i % longStringsSize]);
            value::ValueGuard guard(owned);
            // Ensure pointer comparisons are meaningful (i.e. we're not in the small-string inline
            // representation).
            ASSERT_GT(value::getStringLength(owned.first, owned.second),
                      value::kSmallStringMaxLength);
            row.reset(i, value::TagValueView{owned.first, owned.second});
            auto copied = row.copyOrMoveValue(i);
            ASSERT_NE(value::getRawStringView(owned.first, owned.second),
                      value::getRawStringView(copied.tag(), copied.value()));
            verifyValue(row, i, copyValue(owned));
        }
        // 'row' holds only views, so its destructor must NOT free the strings;
        // the ValueGuards free each string exactly once (a wrongful own would also
        // trip ASAN here as a double-free).
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
        testResetView();

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
        row.reset(idx, value::TagValueMaybeOwned::fromRaw(owned, p.first, p.second));
    }

    void verifyValue(RowType& row, int idx, TypedValue p) {
        value::TagValueOwned pOwner = value::TagValueOwned::fromRaw(p);
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
