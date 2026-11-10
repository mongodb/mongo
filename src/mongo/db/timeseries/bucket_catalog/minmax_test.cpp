// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/unittest/unittest.h"

#include <iterator>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo::timeseries::bucket_catalog {
namespace {
using namespace std::literals::string_view_literals;
using Entry = FlatBSONStore<MinMaxElement, BSONElementValueBuffer>::Entry;

std::string concatFieldNames(const MinMaxStore::Obj& obj) {
    return std::accumulate(
        obj.begin(), obj.end(), std::string(), [](std::string accum, const MinMaxElement& elem) {
            return std::move(accum) + std::string(elem.fieldName());
        });
}

int64_t getElementSize(std::string fieldName,
                       boost::optional<BSONElement> minElem,
                       boost::optional<BSONElement> maxElem) {
    size_t minDataSize = minElem
        ? sizeof(BSONElementValueBuffer) + minElem->size() - fieldName.size()
        : sizeof(BSONElementValueBuffer);
    size_t maxDataSize = maxElem
        ? sizeof(BSONElementValueBuffer) + maxElem->size() - fieldName.size()
        : sizeof(BSONElementValueBuffer);
    return fieldName.capacity() + minDataSize + maxDataSize;
}

int64_t getEntrySize(std::string fieldName,
                     boost::optional<BSONElement> minElem,
                     boost::optional<BSONElement> maxElem) {
    return sizeof(Entry) + getElementSize(fieldName, minElem, maxElem);
}

int64_t emptyStoreSize() {
    static const std::string emptyFieldName;
    return getEntrySize(emptyFieldName, boost::none, boost::none);
}

int64_t emptyMinMaxSize() {
    return sizeof(FlatBSONStore<MinMaxElement, BSONElementValueBuffer>) + emptyStoreSize();
}

TEST(MinMax, Insert) {
    tracking::Context trackingContext;
    MinMaxStore minmax{trackingContext};

    // No subelements to start
    auto obj = minmax.root();
    EXPECT_EQ(std::distance(obj.begin(), obj.end()), 0);

    // We can insert at begin
    {
        auto [inserted, end] = obj.insert(obj.begin(), "b");
        ASSERT(obj.begin() == inserted);
        ASSERT(obj.end() == end);
        EXPECT_EQ(std::distance(inserted, end), 1);
        EXPECT_EQ(inserted->fieldName(), "b");

        // parent should be pointing to obj
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // Insert when there is already something there
    {
        auto [inserted, end] = obj.insert(obj.begin(), "a");
        ASSERT(obj.begin() == inserted);
        ASSERT(obj.end() == end);
        EXPECT_EQ(std::distance(inserted, end), 2);
        EXPECT_EQ(inserted->fieldName(), "a");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());

        // Validate the existing element
        ++inserted;
        EXPECT_EQ(inserted->fieldName(), "b");
        EXPECT_EQ(concatFieldNames(obj), "ab");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // Insert at and when there is already something there
    {
        auto [inserted, end] = obj.insert(obj.end(), "d");
        ASSERT(obj.end() == end);
        EXPECT_EQ(std::distance(inserted, end), 1);
        EXPECT_EQ(std::distance(obj.begin(), obj.end()), 3);
        EXPECT_EQ(inserted->fieldName(), "d");
        EXPECT_EQ(concatFieldNames(obj), "abd");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // We can also insert at the middle
    {
        auto it = obj.begin();
        ++it;
        ++it;
        auto [inserted, end] = obj.insert(it, "c");
        EXPECT_EQ(concatFieldNames(obj), "abcd");
        EXPECT_EQ(std::distance(inserted, end), 2);
        EXPECT_EQ(std::distance(obj.begin(), obj.end()), 4);
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());

        // Validate last element that it got parent updated
        ++inserted;
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }
}

TEST(MinMax, MinMaxNoUpdatesAfterFullMinMax) {
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());

    minMaxObj.update(BSON("a" << 1 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 1)));
}

TEST(MinMax, MinMaxNoUpdatesAfterFullMinMaxNested) {
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;

    auto obj = BSON("a" << BSON("z" << 1) << "b" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)));
    minMaxObj.update(obj, "_meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), obj);
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), obj);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj{});
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj{});

    minMaxObj.update(
        BSON("a" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)) << "b" << BSON("z" << 1)),
        "_meta"sv,
        strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("b" << BSON("z" << 1))));
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(),
                      BSON("u" << BSON("a" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)))));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj{});
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj{});
}

TEST(MinMax, MinMaxInitialUpdates) {
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 2 << "b" << 3)));

    minMaxObj.update(BSON("a" << 1 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 1)));
}

TEST(MinMax, MinMaxMixedUpdates) {
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSON("u" << BSON("a" << 2 << "b" << 3)));
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), BSON("a" << 2 << "b" << 3));

    minMaxObj.update(BSON("a" << 5 << "b" << 3 << "meta" << 4), "meta"sv, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSON("u" << BSON("a" << 5)));
}

TEST(MinMax, SubObjInsert) {
    tracking::Context trackingContext;
    MinMaxStore minmax{trackingContext};
    auto obj = minmax.root();
    auto [inserted, _] = obj.insert(obj.end(), "b");

    auto subobjB = obj.object(inserted);
    EXPECT_EQ(std::distance(subobjB.begin(), subobjB.end()), 0);
    ASSERT(obj.begin() != subobjB.begin());
    ASSERT(obj.end() == subobjB.end());
    ASSERT(obj.begin() == subobjB.parent().begin());

    subobjB.insert(subobjB.begin(), "1");
    subobjB.insert(subobjB.end(), "3");
    obj = subobjB.parent();

    EXPECT_EQ(concatFieldNames(obj), "b");
    EXPECT_EQ(concatFieldNames(obj.object(obj.begin())), "13");

    obj.insert(obj.end(), "c");
    EXPECT_EQ(concatFieldNames(obj), "bc");
    EXPECT_EQ(concatFieldNames(obj.object(obj.begin())), "13");

    std::tie(inserted, _) = obj.insert(obj.begin(), "a");
    EXPECT_EQ(concatFieldNames(obj), "abc");

    subobjB = obj.object(std::next(inserted));
    EXPECT_EQ(concatFieldNames(subobjB), "13");

    // Insert in subobj and check that the last element in obj 'c' got its parent updated.
    std::tie(inserted, _) = subobjB.insert(std::next(subobjB.begin()), "2");
    obj = subobjB.parent();
    EXPECT_EQ(concatFieldNames(obj), "abc");
    EXPECT_EQ(concatFieldNames(subobjB), "123");
    auto itC = obj.begin();
    std::advance(itC, 2);
    EXPECT_EQ(itC->fieldName(), "c");
    ASSERT(obj.object(itC).parent().iterator() == obj.iterator());

    // Create a third level, validate that all iterators is updated
    auto subobjB2 = obj.object(std::next(obj.object(std::next(obj.begin())).begin()));
    subobjB2.insert(subobjB2.begin(), "x");
    subobjB = subobjB2.parent();
    obj = subobjB.parent();
    EXPECT_EQ(concatFieldNames(obj), "abc");
    EXPECT_EQ(concatFieldNames(subobjB), "123");
    EXPECT_EQ(concatFieldNames(subobjB2), "x");
    itC = obj.begin();
    std::advance(itC, 2);
    EXPECT_EQ(itC->fieldName(), "c");
    ASSERT(obj.object(itC).parent().iterator() == obj.iterator());
}

TEST(MinMax, Search) {
    tracking::Context trackingContext;
    MinMaxStore minmax{trackingContext};
    auto obj = minmax.root();
    obj.insert(obj.end(), "a");
    obj.insert(obj.end(), "b");
    obj.insert(obj.end(), "c");
    obj.insert(obj.end(), "d");
    EXPECT_EQ(concatFieldNames(obj), "abcd");

    EXPECT_EQ(obj.search(obj.begin(), "a")->fieldName(), "a");
    EXPECT_EQ(obj.search(obj.begin(), "c")->fieldName(), "c");
    EXPECT_EQ(obj.search(obj.begin(), "d")->fieldName(), "d");
    ASSERT(obj.search(obj.begin(), "e") == obj.end());
    ASSERT(obj.search(std::next(obj.begin()), "a") == obj.end());

    auto itC = std::next(std::next(obj.begin()));
    ASSERT(obj.search(obj.begin(), itC, "d") == itC);
}

TEST(MinMax, SearchLookupMap) {
    tracking::Context trackingContext;
    MinMaxStore minmax{trackingContext};
    auto obj = minmax.root();

    for (int i = 0; i < 100; ++i) {
        obj.insert(obj.end(), std::to_string(i));
    }

    // Trigger lookup map to be created by requiring a long search
    EXPECT_EQ(obj.search(obj.begin(), "99")->fieldName(), "99");

    // When lookup map exists we find things outside of the provided range
    EXPECT_EQ(obj.search(std::next(obj.begin()), "0")->fieldName(), "0");

    // Provided last is still respected when something is not found
    auto last = std::next(obj.begin());
    ASSERT(obj.search(obj.begin(), last, "100") == last);

    // Map based search is still accurate after inserts
    obj.insert(obj.begin(), "x");
    EXPECT_EQ(obj.search(obj.begin(), "50")->fieldName(), "50");
}

TEST(MinMax, DuplicateFieldNamesWithLookupMap) {
    tracking::Context trackingContext;
    MinMaxStore minmax{trackingContext};
    auto obj = minmax.root();

    // Insert 12 (kMaxLinearSearchLength) distinct fields ("0".."11") followed by two duplicate "a"
    // entries. This will trigger the lookup map internally in flat_bson.
    for (int i = 0; i < 12; ++i) {
        obj.insert(obj.end(), std::to_string(i));
    }
    obj.insert(obj.end(), "a");
    obj.insert(obj.end(), "a");

    // Try to search for "a", this will trigger the lookup map internally in flat_bson as we fail to
    // find it within 'kMaxLinearSearchLength' attempts. The map cannot contain duplicates so this
    // search is well defined and throws.
    ASSERT_THROWS(obj.search(obj.begin(), "a"), AssertionException);

    // Try to insert another duplicate which will throw earlier as the lookup map exists and needs
    // to be maintained.
    obj.insert(obj.begin(), "x");
    ASSERT_THROWS(obj.insert(obj.begin(), "x"), AssertionException);

    // Ensure that the _entries vector is still well-formed by checking that searching for each
    // field 'field', using the offset values from before the aborted duplicate insert, still
    // retrieves the correct field 'field'.
    for (int i = 0; i < 12; ++i) {
        auto field = std::to_string(i);
        auto it = obj.search(obj.begin(), field);
        ASSERT(it != obj.end());
        ASSERT_EQ(it->fieldName(), field);
    }

    // Searching for "a" or "x" is possible as we inserted one of them into the map.
    auto found = obj.search(obj.begin(), "a");
    ASSERT(found != obj.end());
    ASSERT_EQ(found->fieldName(), "a");

    found = obj.search(obj.begin(), "x");
    ASSERT(found != obj.end());
    ASSERT_EQ(found->fieldName(), "x");
}

TEST(MinMax, ArraySlotReuseDoesNotCorruptLookupMap) {
    // Regression test for a use-after-free reachable from strict time-series bucket validation.
    //
    // FlatBSONStore keeps array element entries (which share one reserved field name) in the
    // same positional child list as named object fields. When an object field was merged onto an
    // array placeholder entry after the parent's fast lookup map had been built, the entry was
    // renamed without updating the map. The map then missed the new name, a later insert created
    // a duplicate entry with that name, and the insert offset fixup counted the shared name
    // twice, so the map slot drifted onto a different entry. From then on, lookups for that name
    // returned the wrong entry, and with enough drift an out-of-subtree position whose "value"
    // the bucket validator could be driven into resizing and freeing under another entry.
    //
    // This replays the minimal sequence through the public API and asserts on the observable
    // misrouting: 'x' must resolve to a single entry holding the minimum of its own values, and
    // 'z' must keep its own minimum.
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;

    auto wideObj = [](int value) {
        BSONObjBuilder builder;
        for (int i = 0; i <= 12; ++i) {
            builder.append("f" + std::to_string(i), value);
        }
        return builder.obj();
    };

    // Thirteen fields, more than the linear search limit, so that the deep search miss below
    // builds the lookup map.
    minMaxObj.update(BSON("m" << wideObj(0)), boost::none, strCmp);
    // The array makes 'm' an array on the max side and appends an array placeholder entry that
    // shares the child run with the named fields.
    minMaxObj.update(
        BSON("m" << BSON_ARRAY(0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0)),
        boost::none,
        strCmp);
    // Searching for 'z' misses past the linear search limit and builds the lookup map, then 'z'
    // is inserted at the front.
    minMaxObj.update(BSON("m" << BSON("z" << 1)), boost::none, strCmp);
    // The lock-step walk lands on the placeholder exactly when processing 'x' and claims it,
    // leaving the lookup map stale on affected builds.
    minMaxObj.update(BSON("m" <<
                          [&] {
                              BSONObjBuilder builder;
                              builder.append("z", 1);
                              builder.appendElements(wideObj(0));
                              builder.append("x", 5);
                              return builder.obj();
                          }()),
                     boost::none,
                     strCmp);
    // The stale map has no key for 'x', so this inserts a duplicate 'x' entry and the insert
    // fixup counts the shared 'x' name twice, leaving the map slot for 'x' pointing at 'z'.
    minMaxObj.update(BSON("m" << BSON("x" << 7)), boost::none, strCmp);
    // A consistent map resolves 'x' to its own entry and lowers its minimum to 0. The stale map
    // resolves 'x' to the 'z' entry and clobbers z's minimum instead.
    minMaxObj.update(BSON("m" << BSON("z" << 5 << "x" << 0)), boost::none, strCmp);

    BSONObj minObj = minMaxObj.min();
    BSONObj minM = minObj.getObjectField("m");
    int xCount = 0;
    for (auto&& elem : minM) {
        if (elem.fieldNameStringData() == "x") {
            ++xCount;
        }
    }
    ASSERT_EQ(xCount, 1);
    ASSERT_EQ(minM.getIntField("x"), 0);
    ASSERT_EQ(minM.getIntField("z"), 1);

    // The max side keeps the array shape from the second update, since objects sort below
    // arrays in canonical order and no later object update can win the max.
    BSONObj maxObj = minMaxObj.max();
    ASSERT_BSONOBJ_EQ(
        BSON("m" << BSON_ARRAY(0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0 << 0)),
        maxObj);
}

TEST(MinMax, ClaimBeforeLookupMapCreationStaysConsistent) {
    // Companion to ArraySlotReuseDoesNotCorruptLookupMap for the second case: the placeholder
    // claim happens while no fast lookup map exists yet. The map is created later from the
    // names the entries have at that point, so it starts out consistent.
    tracking::Context trackingContext;
    MinMax minMaxObj{trackingContext};
    const auto* strCmp = &simpleStringDataComparator;

    // Five array placeholder entries.
    minMaxObj.update(BSON("m" << BSON_ARRAY(0 << 0 << 0 << 0 << 0)), boost::none, strCmp);
    // Claims three of them. Five children is below the linear search limit, so no map exists.
    minMaxObj.update(BSON("m" << BSON("a" << 1 << "b" << 2 << "c" << 3)), boost::none, strCmp);
    // Claims the remaining two placeholders and appends nine named fields, still with no map.
    minMaxObj.update(BSON("m" <<
                          [] {
                              BSONObjBuilder builder;
                              builder.append("a", 1);
                              builder.append("b", 2);
                              builder.append("c", 3);
                              for (int i = 0; i <= 9; ++i) {
                                  builder.append("d" + std::to_string(i), 0);
                              }
                              builder.append("d10", 42);
                              return builder.obj();
                          }()),
                     boost::none,
                     strCmp);
    // A miss over 14 named children exhausts the linear search limit and builds the map from
    // the current, post-rename names.
    minMaxObj.update(BSON("m" << BSON("zz" << 9)), boost::none, strCmp);
    // This lookup resolves through the map. If the map were out of sync with the entries, the
    // update would land on the wrong field.
    minMaxObj.update(BSON("m" << BSON("d10" << 0)), boost::none, strCmp);

    BSONObj minObj = minMaxObj.min();
    BSONObj minM = minObj.getObjectField("m");
    std::set<std::string> names;
    for (auto&& elem : minM) {
        ASSERT(names.insert(std::string{elem.fieldNameStringData()}).second);
    }
    ASSERT_EQ(names.size(), 15);
    ASSERT_EQ(minM.getIntField("a"), 1);
    ASSERT_EQ(minM.getIntField("b"), 2);
    ASSERT_EQ(minM.getIntField("c"), 3);
    ASSERT_EQ(minM.getIntField("zz"), 9);
    ASSERT_EQ(minM.getIntField("d10"), 0);

    // The max side keeps the array shape: the five claimed entries carry the array values and
    // the named entries appended by object updates have no max value and are skipped.
    BSONObj maxObj = minMaxObj.max();
    ASSERT_BSONOBJ_EQ(BSON("m" << BSON_ARRAY(0 << 0 << 0 << 0 << 0)), maxObj);
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
