// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_state.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mongo::sbe {
namespace {

struct Counts {
    int live() const {
        return constructed - destroyed;
    }

    int constructed = 0;
    int destroyed = 0;
    int misaligned = 0;
};

Counts counts;
std::function<void()> onConstruct;

template <size_t align, size_t size = align>
struct alignas(align) Counted {
    Counted() {
        if (onConstruct) {
            onConstruct();
        }
        ++counts.constructed;
        if (reinterpret_cast<uintptr_t>(this) % align != 0) {
            ++counts.misaligned;
        }
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    ~Counted() {
        ++counts.destroyed;
    }
    std::byte bytes[size];
};

template <size_t align, size_t size = align>
struct alignas(align) Plain {
    std::byte bytes[size];
};

struct Defaults {
    int64_t count = 7;
    double total = 1.5;
};

class CompactHashAggStateTest : public unittest::Test {
protected:
    void setUp() override {
        counts = {};
    }

    void tearDown() override {
        onConstruct = nullptr;
    }

    /**
     * Makes the Counted constructor throw after 'n' more constructions succeed. Later
     * constructions succeed.
     */
    static void failConstructionAfter(int n) {
        onConstruct = [n, constructed = 0]() mutable {
            uassert(ErrorCodes::InternalError, "constructor failed", constructed++ != n);
        };
    }

    static value::MaterializedRow key(std::string_view text) {
        value::MaterializedRow row{1};
        row.reset(0, value::TagValueOwned::fromRaw(value::makeNewString(text)));
        return row;
    }

    static std::string keyText(const value::MaterializedRow& row) {
        auto view = row.getViewOfValue(0);
        return std::string{value::getStringView(view.tag, view.value)};
    }

    static value::MaterializedRow key(int32_t number) {
        value::MaterializedRow row{1};
        row.reset(0, value::TagValueView::numberInt32(number));
        return row;
    }
};

TEST_F(CompactHashAggStateTest, LayoutPadsEachStateToItsOwnAlignment) {
    using Bytes3 = Plain<1, 3>;
    using Bytes4 = Plain<4, 4>;
    CompactStateLayout layout;
    ASSERT_EQ(layout.add<Bytes3>().offset(), 0);
    ASSERT_EQ(layout.add<Bytes3>().offset(), 3);
    ASSERT_EQ(layout.add<Bytes4>().offset(), 8);
    ASSERT_EQ(layout.add<Plain<8>>().offset(), 16);
    ASSERT_EQ(layout.size(), 24);
    ASSERT_EQ(layout.alignment(), 8);
    ASSERT_TRUE(layout.triviallyDestructible());
}

TEST_F(CompactHashAggStateTest, TableAlignsStatesBeyondTheDefaultNewAlignment) {
    static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ < 32);
    CompactStateLayout layout;
    layout.add<Counted<1>>();
    ASSERT_EQ(layout.add<Counted<32>>().offset(), 32);
    ASSERT_EQ(layout.size(), 64);
    ASSERT_EQ(layout.alignment(), 32);
    {
        CompactStateTable table{std::move(layout)};
        for (int32_t number = 0; number < 16; ++number) {
            std::byte* buffer = table.findOrInsert(key(number))->second;
            ASSERT_EQ(reinterpret_cast<uintptr_t>(buffer) % 32, 0);
        }
    }
    ASSERT_EQ(counts.misaligned, 0);
    ASSERT_EQ(counts.live(), 0);
}

TEST_F(CompactHashAggStateTest, TableConstructsAlignedStates) {
    CompactStateLayout layout;
    layout.add<Counted<1>>();
    layout.add<Counted<4>>();
    auto defaults = layout.add<Defaults>();
    static_assert(std::is_same_v<decltype(defaults)::value_type, Defaults>);
    layout.add<Counted<8>>();

    CompactStateTable table{std::move(layout)};
    ASSERT_FALSE(table.layout().triviallyDestructible());
    std::byte* buffer = table.findOrInsert(key(1))->second;
    ASSERT_EQ(counts.live(), 3);
    ASSERT_EQ(counts.misaligned, 0);
    ASSERT_EQ(defaults(buffer).count, 7);
    ASSERT_EQ(defaults(buffer).total, 1.5);

    const std::byte* constBuffer = buffer;
    static_assert(std::is_same_v<decltype(defaults(constBuffer)), const Defaults&>);
    ASSERT_EQ(&defaults(constBuffer), &defaults(buffer));

    table.clear();
    ASSERT_EQ(counts.live(), 0);
}

TEST_F(CompactHashAggStateTest, EmptyLayoutInsertsEmptyStates) {
    CompactStateLayout layout;
    ASSERT_EQ(layout.size(), 0);
    CompactStateTable table{std::move(layout)};
    ASSERT_EQ(table.findOrInsert(key(1))->second, nullptr);
}

TEST_F(CompactHashAggStateTest, TableDestroysBuiltStatesWhenConstructorThrows) {
    CompactStateLayout layout;
    layout.add<Counted<8>>();
    layout.add<Counted<8>>();
    CompactStateTable table{std::move(layout)};

    failConstructionAfter(1);
    ASSERT_THROWS_CODE(table.findOrInsert(key(1)), DBException, ErrorCodes::InternalError);
    ASSERT_EQ(counts.live(), 0);
    ASSERT_TRUE(table.empty());
}

TEST_F(CompactHashAggStateTest, TableDestroysBuiltStatesAmongTrivialOnesWhenConstructorThrows) {
    CompactStateLayout layout;
    layout.add<Plain<8>>();
    layout.add<Counted<8>>();
    layout.add<Plain<8>>();
    layout.add<Counted<8>>();
    layout.add<Counted<8>>();
    CompactStateTable table{std::move(layout)};

    failConstructionAfter(2);
    ASSERT_THROWS_CODE(table.findOrInsert(key(1)), DBException, ErrorCodes::InternalError);
    ASSERT_EQ(counts.live(), 0);
    ASSERT_TRUE(table.empty());
}

TEST_F(CompactHashAggStateTest, TableConstructsOnInsertAndDestroysOnClear) {
    CompactStateLayout layout;
    layout.add<Counted<8>>();
    {
        CompactStateTable table{std::move(layout)};
        for (int32_t number : {1, 2, 1, 3, 2}) {
            table.findOrInsert(key(number));
        }
        ASSERT_EQ(table.size(), 3);
        ASSERT_EQ(counts.live(), 3);

        table.clear();
        ASSERT_TRUE(table.empty());
        ASSERT_EQ(counts.live(), 0);

        table.findOrInsert(key(4));
        table.findOrInsert(key(5));
        ASSERT_EQ(counts.live(), 2);
    }
    ASSERT_EQ(counts.live(), 0);
}

TEST_F(CompactHashAggStateTest, TableDestroysEachStateOnceAcrossRehashes) {
    CompactStateLayout layout;
    layout.add<Counted<8>>();
    {
        CompactStateTable table{std::move(layout)};
        for (int32_t number = 0; number < 1000; ++number) {
            table.findOrInsert(key(number));
        }
        ASSERT_EQ(counts.live(), 1000);
    }
    ASSERT_EQ(counts.live(), 0);
}

TEST_F(CompactHashAggStateTest, TableFindOrInsertReturnsTheSameStates) {
    CompactStateLayout layout;
    auto position = layout.add<Defaults>();
    CompactStateTable table{std::move(layout)};

    position(table.findOrInsert(key(1))->second).count = 42;
    table.findOrInsert(key(2));
    auto it = table.findOrInsert(key(1));
    ASSERT_EQ(position(it->second).count, 42);
    ASSERT_EQ(value::bitcastTo<int32_t>(it->first.getViewOfValue(0).value), 1);
}

TEST_F(CompactHashAggStateTest, TableErasesEntryWhenConstructorThrows) {
    CompactStateLayout layout;
    layout.add<Counted<8>>();
    CompactStateTable table{std::move(layout)};
    table.findOrInsert(key(1));

    failConstructionAfter(0);
    ASSERT_THROWS_CODE(table.findOrInsert(key(2)), DBException, ErrorCodes::InternalError);
    ASSERT_EQ(table.size(), 1);
    ASSERT_EQ(counts.live(), 1);

    table.findOrInsert(key(2));
    ASSERT_EQ(table.size(), 2);
}

TEST_F(CompactHashAggStateTest, LayoutHeapSizeCountsItsEntries) {
    ASSERT_EQ(CompactStateLayout{}.heapSize(), 0);
    CompactStateLayout layout;
    layout.add<Plain<8>>();
    layout.add<Counted<8>>();
    ASSERT_GT(layout.heapSize(), 0);
}

TEST_F(CompactHashAggStateTest, TableOwnsItsKeys) {
    CompactStateLayout layout;
    auto position = layout.add<Defaults>();
    CompactStateTable table{std::move(layout)};
    std::string text(100, 'k');
    {
        auto original = value::TagValueOwned::fromRaw(value::makeNewString(text));
        value::MaterializedRow borrowed{1};
        borrowed.reset(0, original.view());
        auto it = table.findOrInsert(borrowed);
        ASSERT_NE(it->first.getViewOfValue(0).value, original.value());
        position(it->second).count = 42;
    }
    auto it = table.findOrInsert(key(text));
    ASSERT_EQ(table.size(), 1);
    ASSERT_EQ(position(it->second).count, 42);
    ASSERT_EQ(keyText(it->first), text);
}

TEST_F(CompactHashAggStateTest, TableMatchesKeysWithTheCollator) {
    CompactStateLayout foldedLayout;
    foldedLayout.add<Defaults>();
    CompactStateLayout exactLayout;
    exactLayout.add<Defaults>();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    CompactStateTable folded{std::move(foldedLayout),
                             value::MaterializedRowHasher(&collator),
                             value::MaterializedRowEq(&collator)};
    CompactStateTable exact{std::move(exactLayout)};
    for (std::string_view text : {"Apple", "apple"}) {
        folded.findOrInsert(key(text));
        exact.findOrInsert(key(text));
    }
    ASSERT_EQ(folded.size(), 1);
    ASSERT_EQ(keyText(folded.begin()->first), "Apple");
    ASSERT_EQ(exact.size(), 2);
}

}  // namespace
}  // namespace mongo::sbe
