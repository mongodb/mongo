/**
 *    Copyright 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/storage/biggie/store.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace biggie {
namespace {

using StringStore = Store<std::string, std::string>;
using value_type = StringStore::value_type;

class StoreTest : public unittest::Test {
protected:
    StringStore thisStore;
    StringStore otherStore;
    StringStore baseStore;
    StringStore expected;
};

TEST_F(StoreTest, InsertTest) {
    value_type value1 = std::make_pair("1", "foo");
    std::pair<StringStore::iterator, bool> res = thisStore.insert(value_type(value1));
    ASSERT_TRUE(res.second);
    ASSERT_TRUE(*res.first == value1);
}

TEST_F(StoreTest, EmptyTest) {
    value_type value1 = std::make_pair("1", "foo");
    ASSERT_TRUE(thisStore.empty());

    thisStore.insert(value_type(value1));
    ASSERT_FALSE(thisStore.empty());
}

TEST_F(StoreTest, SizeTest) {
    value_type value1 = std::make_pair("1", "foo");
    auto expected1 = StringStore::size_type(0);
    ASSERT_EQ(thisStore.size(), expected1);

    thisStore.insert(value_type(value1));
    auto expected2 = StringStore::size_type(1);
    ASSERT_EQ(thisStore.size(), expected2);
}

TEST_F(StoreTest, ClearTest) {
    value_type value1 = std::make_pair("1", "foo");

    thisStore.insert(value_type(value1));
    ASSERT_FALSE(thisStore.empty());

    thisStore.clear();
    ASSERT_TRUE(thisStore.empty());
}

TEST_F(StoreTest, EraseTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    auto expected1 = StringStore::size_type(2);
    ASSERT_EQ(thisStore.size(), expected1);

    thisStore.erase(value1.first);
    auto expected2 = StringStore::size_type(1);
    ASSERT_EQ(thisStore.size(), expected2);

    thisStore.erase("3");
    ASSERT_EQ(thisStore.size(), expected2);
}

TEST_F(StoreTest, FindTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    auto expected = StringStore::size_type(2);
    ASSERT_EQ(thisStore.size(), expected);

    StringStore::iterator iter1 = thisStore.find(value1.first);
    ASSERT_TRUE(*iter1 == value1);

    StringStore::iterator iter2 = thisStore.find("3");
    ASSERT_TRUE(iter2 == thisStore.end());
}

TEST_F(StoreTest, DataSizeTest) {
    std::string str1 = "foo";
    std::string str2 = "bar65";

    value_type value1 = std::make_pair("1", str1);
    value_type value2 = std::make_pair("2", str2);
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    ASSERT_EQ(thisStore.dataSize(), str1.size() + str2.size());
}

TEST_F(StoreTest, DistanceTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");
    value_type value3 = std::make_pair("3", "foo");
    value_type value4 = std::make_pair("4", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    StringStore::iterator begin = thisStore.begin();
    StringStore::iterator second = thisStore.begin();
    ++second;
    StringStore::iterator end = thisStore.end();

    ASSERT_EQ(thisStore.distance(begin, end), 4);
    ASSERT_EQ(thisStore.distance(second, end), 3);
}

TEST_F(StoreTest, MergeNoModifications) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value2));

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == thisStore);
}

TEST_F(StoreTest, MergeModifications) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("1", "bar");

    value_type value3 = std::make_pair("3", "baz");
    value_type value4 = std::make_pair("3", "faz");

    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value4));

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value3));

    expected.insert(value_type(value2));
    expected.insert(value_type(value4));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == expected);
}

TEST_F(StoreTest, MergeDeletions) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "moo");
    value_type value3 = std::make_pair("3", "bar");
    value_type value4 = std::make_pair("4", "baz");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value2));
    otherStore.insert(value_type(value3));

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    expected.insert(value_type(value1));
    expected.insert(value_type(value3));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == expected);
}

TEST_F(StoreTest, MergeInsertions) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "foo");
    value_type value3 = std::make_pair("3", "bar");
    value_type value4 = std::make_pair("4", "faz");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value4));

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value2));
    otherStore.insert(value_type(value3));

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));
    expected.insert(value_type(value3));
    expected.insert(value_type(value4));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == expected);
}

TEST_F(StoreTest, MergeEmptyInsertionOther) {
    value_type value1 = std::make_pair("1", "foo");

    otherStore.insert(value_type(value1));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == otherStore);
}

TEST_F(StoreTest, MergeEmptyInsertionThis) {
    value_type value1 = std::make_pair("1", "foo");

    StringStore thisStore;
    thisStore.insert(value_type(value1));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == thisStore);
}

TEST_F(StoreTest, MergeInsertionDeletionModification) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "baz");
    value_type value3 = std::make_pair("3", "bar");
    value_type value4 = std::make_pair("4", "faz");
    value_type value5 = std::make_pair("5", "too");
    value_type value6 = std::make_pair("6", "moo");
    value_type value7 = std::make_pair("1", "modified");
    value_type value8 = std::make_pair("2", "modified2");

    thisStore.insert(value_type(value7));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value5));

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value8));
    otherStore.insert(value_type(value4));
    otherStore.insert(value_type(value6));

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    expected.insert(value_type(value7));
    expected.insert(value_type(value8));
    expected.insert(value_type(value5));
    expected.insert(value_type(value6));

    StringStore merged = thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(merged == expected);
}

TEST_F(StoreTest, MergeConflictingModifications) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("1", "bar");
    value_type value3 = std::make_pair("1", "baz");

    thisStore.insert(value_type(value2));

    otherStore.insert(value_type(value3));

    baseStore.insert(value_type(value1));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(StoreTest, MergeConflictingModifictionOtherAndDeletionThis) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("1", "bar");

    otherStore.insert(value_type(value2));

    baseStore.insert(value_type(value1));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(StoreTest, MergeConflictingModifictionThisAndDeletionOther) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("1", "bar");

    thisStore.insert(value_type(value2));

    baseStore.insert(value_type(value1));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(StoreTest, MergeConflictingInsertions) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("1", "foo");

    thisStore.insert(value_type(value2));

    otherStore.insert(value_type(value1));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(StoreTest, UpperBoundTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");
    value_type value3 = std::make_pair("3", "foo");
    value_type value4 = std::make_pair("5", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    StringStore::iterator iter1 = thisStore.upper_bound(value2.first);
    ASSERT_EQ(iter1->first, "3");
    StringStore::iterator iter2 = thisStore.upper_bound(value4.first);
    ASSERT_TRUE(iter2 == thisStore.end());
}

TEST_F(StoreTest, LowerBoundTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");
    value_type value3 = std::make_pair("3", "foo");
    value_type value4 = std::make_pair("5", "bar");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    StringStore::iterator iter1 = thisStore.lower_bound(value2.first);
    ASSERT_EQ(iter1->first, "2");
    StringStore::iterator iter2 = thisStore.lower_bound("7");
    ASSERT_TRUE(iter2 == thisStore.end());
}

TEST_F(StoreTest, ReverseIteratorTest) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "bar");
    value_type value3 = std::make_pair("3", "foo");
    value_type value4 = std::make_pair("4", "bar");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 4;
    for (auto iter = thisStore.rbegin(); iter != thisStore.rend(); ++iter) {
        ASSERT_EQ(iter->first, std::to_string(cur));
        --cur;
    }
    ASSERT_EQ(cur, 0);
}
}  // namespace
}  // namespace biggie
}  // namespace mongo
