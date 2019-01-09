
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/biggie/store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace biggie {

using value_type = StringStore::value_type;

class RadixStoreTest : public unittest::Test {
public:
    virtual ~RadixStoreTest() {
        checkValid(thisStore);
        checkValid(parallelStore);
        checkValid(otherStore);
        checkValid(baseStore);
        checkValid(expected);
    }

    StringStore::Head* getRootAddress() const {
        return thisStore._root.get();
    }

    int getRootCount() const {
        return thisStore._root.use_count();
    }

    bool hasPreviousVersion() const {
        return thisStore._root->hasPreviousVersion();
    }

    void checkValid(StringStore& store) const {
        size_t actualSize = 0;
        size_t actualDataSize = 0;
        std::string lastKey = "";
        for (auto& item : store) {
            ASSERT_GT(item.first, lastKey);
            actualDataSize += item.second.size();
            actualSize++;
        }
        ASSERT_EQ(store.size(), actualSize);
        ASSERT_EQ(store.dataSize(), actualDataSize);
    }

protected:
    StringStore thisStore;
    StringStore parallelStore;
    StringStore otherStore;
    StringStore baseStore;
    StringStore expected;
};

TEST_F(RadixStoreTest, SimpleInsertTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");

    std::pair<StringStore::const_iterator, bool> res = thisStore.insert(value_type(value1));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(1));
    ASSERT_TRUE(res.second);
    ASSERT_TRUE(*res.first == value1);

    res = thisStore.insert(value_type(value2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_TRUE(res.second);
    ASSERT_TRUE(*res.first == value2);

    res = thisStore.insert(value_type(value3));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));
    ASSERT_TRUE(res.second);
    ASSERT_TRUE(*res.first == value3);
}

TEST_F(RadixStoreTest, SimpleIteratorAssignmentTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    otherStore = thisStore;

    StringStore::const_iterator thisIter = thisStore.begin();
    StringStore::const_iterator otherIter = otherStore.begin();

    ASSERT_TRUE(thisIter == otherIter);

    thisIter = otherStore.begin();
    ASSERT_TRUE(thisIter == otherIter);
}

TEST_F(RadixStoreTest, IteratorRevalidateOneTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "foo");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "food");

    thisStore.insert(value_type(value3));
    ASSERT_TRUE(thisIter != thisStore.end());
    ASSERT_EQ((*thisIter).first, "food");
}

TEST_F(RadixStoreTest, IteratorRevalidateTwoTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "foo");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");

    ASSERT_EQ((*thisIter).first, "zebra");
}

TEST_F(RadixStoreTest, IteratorRevalidateThreeTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "foo");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("zebra");

    ASSERT_EQ((*thisIter).first, "food");
}

TEST_F(RadixStoreTest, IteratorRevalidateFourTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");
    thisStore.insert(value_type(value2));

    ASSERT_EQ((*thisIter).first, "grass");
}

TEST_F(RadixStoreTest, IteratorRevalidateFiveTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");
    thisStore.insert(value_type(value1));

    ASSERT_EQ((*thisIter).first, "food");
}

TEST_F(RadixStoreTest, IteratorRevalidateSixTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("food", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.update(value_type(value3));

    ASSERT_EQ((*thisIter).first, "food");
    ASSERT_EQ((*thisIter).second, "3");
}

TEST_F(RadixStoreTest, IteratorRevalidateSevenTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");
    thisStore.erase("grass");

    ASSERT_EQ((*thisIter).first, "zebra");
}

TEST_F(RadixStoreTest, IteratorRevalidateEightTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_iterator thisIter = thisStore.begin();
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");
    thisStore.erase("grass");
    thisStore.erase("zebra");

    ASSERT_TRUE(thisIter == thisStore.end());
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateOneTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "food");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "foo");

    thisStore.insert(value_type(value3));
    ASSERT_TRUE(thisIter != thisStore.rend());
    ASSERT_EQ((*thisIter).first, "foo");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateTwoTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "zebra");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("food");

    ASSERT_EQ((*thisIter).first, "foo");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateThreeTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "zebra");
    thisIter++;
    ASSERT_EQ((*thisIter).first, "food");
    thisStore.erase("foo");

    ASSERT_EQ((*thisIter).first, "food");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateFourTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "zebra");
    thisStore.erase("zebra");
    thisStore.insert(value_type(value2));

    ASSERT_EQ((*thisIter).first, "grass");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateFiveTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "grass");
    thisStore.erase("grass");
    thisStore.insert(value_type(value2));

    ASSERT_EQ((*thisIter).first, "grass");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateSixTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("grass", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "grass");
    thisStore.update(value_type(value3));

    ASSERT_EQ((*thisIter).first, "grass");
    ASSERT_EQ((*thisIter).second, "3");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateSevenTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "zebra");
    thisStore.erase("food");
    thisStore.erase("grass");

    ASSERT_EQ((*thisIter).first, "zebra");
}

TEST_F(RadixStoreTest, ReverseIteratorRevalidateEightTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("grass", "2");
    value_type value3 = std::make_pair("zebra", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore::const_reverse_iterator thisIter = thisStore.rbegin();
    ASSERT_EQ((*thisIter).first, "zebra");
    thisStore.erase("food");
    thisStore.erase("grass");
    thisStore.erase("zebra");

    ASSERT_TRUE(thisIter == thisStore.rend());
}

TEST_F(RadixStoreTest, InsertInCopyFromRootTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fee", "3");
    value_type value4 = std::make_pair("fed", "4");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    otherStore = thisStore;

    std::pair<StringStore::const_iterator, bool> res = otherStore.insert(value_type(value4));
    ASSERT_TRUE(res.second);

    StringStore::const_iterator it1 = thisStore.find(value4.first);
    StringStore::const_iterator it2 = otherStore.find(value4.first);

    ASSERT_TRUE(it1 == thisStore.end());
    ASSERT_TRUE(it2 != otherStore.end());

    StringStore::const_iterator check_this = thisStore.begin();
    StringStore::const_iterator check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'fee'
    // node
    ASSERT_TRUE(check_other->first == value4.first);
    ASSERT_TRUE(check_this->first == value3.first);

    // 'otherStore' should have a 'fee' object.
    check_other++;
    ASSERT_TRUE(check_other->first == value3.first);

    // Both should point to different "fee" nodes due to the insertion of 'fed' splitting
    // the 'fee' node in other.
    ASSERT_NOT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Both should point to the same "foo" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, InsertInBothCopiesTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fee", "3");
    value_type value4 = std::make_pair("fed", "4");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    otherStore = thisStore;

    thisStore.insert(value_type(value3));
    otherStore.insert(value_type(value4));

    StringStore::const_iterator check_this = thisStore.find(value4.first);
    StringStore::const_iterator check_other = otherStore.find(value3.first);

    // 'thisStore' should not have value4 and 'otherStore' should not have value3.
    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());

    check_this = thisStore.begin();
    check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'fee'
    // node
    ASSERT_TRUE(check_this->first == value3.first);
    ASSERT_TRUE(check_other->first == value4.first);
    check_other++;
    check_this++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Both should point to the same "foo" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, InsertTwiceInCopyTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fee", "3");
    value_type value4 = std::make_pair("fed", "4");
    value_type value5 = std::make_pair("food", "5");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    otherStore = thisStore;

    otherStore.insert(value_type(value4));
    otherStore.insert(value_type(value5));

    StringStore::const_iterator check_this = thisStore.begin();
    StringStore::const_iterator check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'fee'
    // node
    ASSERT_TRUE(check_other->first == value4.first);
    ASSERT_TRUE(check_this->first == value3.first);

    // 'otherStore' should have a 'fee' object.
    check_other++;
    ASSERT_TRUE(check_other->first == value3.first);

    // Both should point to different "fee" nodes due to the insertion of 'fed' splitting
    // the 'fee' node in other.
    ASSERT_NOT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Both should point to "foo", but they should be different objects
    ASSERT_TRUE(check_this->first == value1.first);
    ASSERT_TRUE(check_other->first == value1.first);
    ASSERT_TRUE(&*check_this != &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other->first == value5.first);
    check_other++;

    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, InsertInBranchWithSharedChildrenTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fee", "3");
    value_type value4 = std::make_pair("fed", "4");
    value_type value5 = std::make_pair("feed", "5");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    otherStore = thisStore;

    otherStore.insert(value_type(value4));
    otherStore.insert(value_type(value5));

    StringStore::const_iterator check_this = thisStore.begin();
    StringStore::const_iterator check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'fee'
    // node
    ASSERT_TRUE(check_other->first == value4.first);
    ASSERT_TRUE(check_this->first == value3.first);
    check_other++;

    // Both should point to "fee", but they should be different objects
    ASSERT_TRUE(check_this->first == value3.first);
    ASSERT_TRUE(check_other->first == value3.first);
    ASSERT_TRUE(&*check_this != &*check_other);
    check_this++;
    check_other++;

    // Only 'otherStore' should have the 'feed' object
    ASSERT_TRUE(check_other->first == value5.first);
    check_other++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "foo" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, InsertNonLeafNodeInBranchWithSharedChildrenTest) {
    value_type value1 = std::make_pair("fed", "1");
    value_type value2 = std::make_pair("fee", "2");
    value_type value3 = std::make_pair("feed", "3");
    value_type value4 = std::make_pair("fod", "4");
    value_type value5 = std::make_pair("foo", "5");

    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));

    otherStore = thisStore;

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value2));

    StringStore::const_iterator check_this = thisStore.begin();
    StringStore::const_iterator check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'feed'
    // node
    ASSERT_TRUE(check_other->first == value1.first);
    ASSERT_TRUE(check_this->first == value3.first);
    check_other++;

    // Only 'otherStore' should have the 'fee' object
    ASSERT_TRUE(check_other->first == value2.first);
    check_other++;

    // Now both should point to a "feed" node.
    ASSERT_TRUE(check_this->first == check_other->first);

    // The 'feed' nodes should be different due to the insertion of 'fee' splitting the 'feed'
    // node in 'otherStore'
    ASSERT_NOT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "foo" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, FindTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("foozeball", "3");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));

    StringStore::const_iterator iter1 = thisStore.find(value1.first);
    ASSERT_FALSE(iter1 == thisStore.end());
    ASSERT_TRUE(*iter1 == value1);

    StringStore::const_iterator iter2 = thisStore.find(value2.first);
    ASSERT_FALSE(iter2 == thisStore.end());
    ASSERT_TRUE(*iter2 == value2);

    StringStore::const_iterator iter3 = thisStore.find(value3.first);
    ASSERT_FALSE(iter3 == thisStore.end());
    ASSERT_TRUE(*iter3 == value3);

    StringStore::const_iterator iter4 = thisStore.find("fooze");
    ASSERT_TRUE(iter4 == thisStore.end());
}

TEST_F(RadixStoreTest, UpdateTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("foz", "3");
    value_type update = std::make_pair("foo", "test");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore copy(thisStore);
    thisStore.update(value_type(update));

    StringStore::const_iterator it2 = thisStore.begin();
    StringStore::const_iterator copy_it2 = copy.begin();

    // both should point to the same 'bar' object
    ASSERT_EQ(&*it2, &*copy_it2);
    it2++;
    copy_it2++;

    // the 'foo' object should be different
    ASSERT_TRUE(it2->second == "test");
    ASSERT_TRUE(copy_it2->second != "test");
    ASSERT_TRUE(&*copy_it2 != &*it2);
    it2++;
    copy_it2++;

    ASSERT_EQ(&*it2, &*copy_it2);
    it2++;
    copy_it2++;

    ASSERT_TRUE(copy_it2 == copy.end());
    ASSERT_TRUE(it2 == thisStore.end());
}

TEST_F(RadixStoreTest, DuplicateKeyTest) {
    std::string msg1 = "Hello, world!";
    std::string msg2 = msg1 + "!!";
    value_type value1 = std::make_pair("msg", msg1);
    value_type value2 = std::make_pair("msg", msg2);

    ASSERT(thisStore.insert(value_type(value1)).second);
    ASSERT_EQ(thisStore.size(), 1u);
    ASSERT_EQ(thisStore.dataSize(), msg1.size());

    ASSERT(!thisStore.insert(value_type(value2)).second);
    ASSERT_EQ(thisStore.size(), 1u);
    ASSERT_EQ(thisStore.dataSize(), msg1.size());
}

TEST_F(RadixStoreTest, UpdateLeafOnSharedNodeTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("fool", "3");
    value_type upd = std::make_pair("fool", "test");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore copy(thisStore);
    thisStore.update(value_type(upd));

    StringStore::const_iterator it2 = thisStore.begin();
    StringStore::const_iterator copy_it2 = copy.begin();

    // both should point to the same 'bar' object
    ASSERT_EQ(&*it2, &*copy_it2);
    it2++;
    copy_it2++;

    // the 'foo' object should be different but have the same value. This is due to the node being
    // copied since 'fool' was updated
    ASSERT_TRUE(it2->second == "1");
    ASSERT_TRUE(copy_it2->second == "1");
    ASSERT_TRUE(&*copy_it2 != &*it2);
    it2++;
    copy_it2++;

    // the 'fool' object should be different
    ASSERT_TRUE(it2->second == "test");
    ASSERT_TRUE(copy_it2->second != "test");
    ASSERT_TRUE(&*copy_it2 != &*it2);
    it2++;
    copy_it2++;

    ASSERT_TRUE(copy_it2 == copy.end());
    ASSERT_TRUE(it2 == thisStore.end());
}

TEST_F(RadixStoreTest, UpdateSharedBranchNonLeafNodeTest) {
    value_type value1 = std::make_pair("fee", "1");
    value_type value2 = std::make_pair("fed", "2");
    value_type value3 = std::make_pair("feed", "3");
    value_type value4 = std::make_pair("foo", "4");
    value_type value5 = std::make_pair("fod", "5");
    value_type upd_val = std::make_pair("fee", "6");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));

    otherStore = thisStore;

    otherStore.insert(value_type(value2));
    otherStore.update(value_type(upd_val));

    StringStore::const_iterator check_this = thisStore.begin();
    StringStore::const_iterator check_other = otherStore.begin();

    // Only 'otherStore' should have the 'fed' object, whereas thisStore should point to the 'fee'
    // node
    ASSERT_TRUE(check_this->first == value1.first);
    ASSERT_TRUE(check_other->first == value2.first);
    check_other++;

    // 'thisStore' should point to the old 'fee' object whereas 'otherStore' should point to the
    // updated object
    ASSERT_TRUE(check_this->first == value1.first);
    ASSERT_TRUE(check_this->second == value1.second);
    ASSERT_TRUE(check_other->first == value1.first);
    ASSERT_TRUE(check_other->second == upd_val.second);
    ASSERT_TRUE(&*check_this != &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "feed" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "fod" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    // Now both should point to the same "foo" node.
    ASSERT_EQUALS(&*check_this, &*check_other);
    check_this++;
    check_other++;

    ASSERT_TRUE(check_this == thisStore.end());
    ASSERT_TRUE(check_other == otherStore.end());
}

TEST_F(RadixStoreTest, SimpleEraseTest) {
    value_type value1 = std::make_pair("abc", "1");
    value_type value2 = std::make_pair("def", "4");
    value_type value3 = std::make_pair("ghi", "5");
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));

    StringStore::size_type success = thisStore.erase(value1.first);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));

    auto iter = thisStore.begin();
    ASSERT_TRUE(*iter == value2);
    ++iter;
    ASSERT_TRUE(*iter == value3);
    ++iter;
    ASSERT_TRUE(iter == thisStore.end());

    ASSERT_FALSE(thisStore.erase("jkl"));
}

TEST_F(RadixStoreTest, EraseNodeWithUniquelyOwnedParent) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fed", "3");
    value_type value4 = std::make_pair("feed", "4");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value4));

    otherStore = thisStore;
    otherStore.insert(value_type(value3));

    StringStore::size_type success = otherStore.erase(value4.first);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));
    ASSERT_EQ(otherStore.size(), StringStore::size_type(3));

    auto this_it = thisStore.begin();
    auto other_it = otherStore.begin();

    // 'thisStore' should still have the 'feed' object whereas 'otherStore' should point to the
    // 'fed' object.
    ASSERT_TRUE(this_it->first == value4.first);
    ASSERT_TRUE(other_it->first == value3.first);
    this_it++;
    other_it++;

    // 'thisStore' and 'otherStore' should point to the same 'fod' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    // 'thisStore' and 'otherStore' should point to the same 'foo' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    ASSERT_TRUE(this_it == thisStore.end());
    ASSERT_TRUE(other_it == otherStore.end());
}

TEST_F(RadixStoreTest, EraseNodeWithSharedParent) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value5 = std::make_pair("feed", "5");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value5));

    otherStore = thisStore;

    StringStore::size_type success = otherStore.erase(value5.first);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));
    ASSERT_EQ(otherStore.size(), StringStore::size_type(2));

    auto this_it = thisStore.begin();
    auto other_it = otherStore.begin();

    // 'thisStore' should still have the 'feed' object whereas 'otherStore' should point to the
    // 'fod' object.
    ASSERT_TRUE(this_it->first == value5.first);
    ASSERT_TRUE(other_it->first == value2.first);
    this_it++;

    // Both iterators should point to the same 'fod' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    // 'thisStore' and 'otherStore' should point to the same 'foo' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    ASSERT_TRUE(this_it == thisStore.end());
    ASSERT_TRUE(other_it == otherStore.end());
}

TEST_F(RadixStoreTest, EraseNonLeafNodeWithSharedParent) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fee", "3");
    value_type value5 = std::make_pair("feed", "5");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value5));

    otherStore = thisStore;

    bool success = otherStore.erase(value3.first);

    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(4));
    ASSERT_EQ(otherStore.size(), StringStore::size_type(3));

    auto this_it = thisStore.begin();
    auto other_it = otherStore.begin();

    // 'thisStore' should still have the 'fee' object whereas 'otherStore' should point to the
    // 'feed' object.
    ASSERT_TRUE(this_it->first == value3.first);
    ASSERT_TRUE(other_it->first == value5.first);

    // 'thisStore' should have a 'feed' node.
    this_it++;
    ASSERT_TRUE(this_it->first == value5.first);

    // Both iterators should point to different 'feed' objects because erasing 'fee' from
    // 'otherStore' caused 'feed' to be compressed.
    ASSERT_NOT_EQUALS(&*this_it, &*other_it);
    this_it++;
    other_it++;

    // 'thisStore' and 'otherStore' should point to the same 'fod' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    // 'thisStore' and 'otherStore' should point to the same 'foo' object.
    ASSERT_TRUE(&*this_it == &*other_it);
    this_it++;
    other_it++;

    ASSERT_TRUE(this_it == thisStore.end());
    ASSERT_TRUE(other_it == otherStore.end());
}

TEST_F(RadixStoreTest, ErasePrefixOfAnotherKeyOfCopiedStoreTest) {
    std::string prefix = "bar";
    std::string prefix2 = "barrista";
    value_type value1 = std::make_pair(prefix, "1");
    value_type value2 = std::make_pair(prefix2, "2");
    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    thisStore = baseStore;
    StringStore::size_type success = thisStore.erase(prefix);

    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(1));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(2));
    StringStore::const_iterator iter = thisStore.find(prefix2);
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, prefix2);
}

TEST_F(RadixStoreTest, ErasePrefixOfAnotherKeyTest) {
    std::string prefix = "bar";
    std::string otherKey = "barrista";
    value_type value1 = std::make_pair(prefix, "2");
    value_type value2 = std::make_pair(otherKey, "3");
    value_type value3 = std::make_pair("foz", "4");
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));

    StringStore::size_type success = thisStore.erase(prefix);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    StringStore::const_iterator iter = thisStore.find(otherKey);
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, otherKey);
}

TEST_F(RadixStoreTest, EraseKeyWithPrefixStillInStoreTest) {
    std::string key = "barrista";
    std::string prefix = "bar";
    value_type value1 = std::make_pair(prefix, "2");
    value_type value2 = std::make_pair(key, "3");
    value_type value3 = std::make_pair("foz", "4");
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));

    StringStore::size_type success = thisStore.erase(key);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    StringStore::const_iterator iter = thisStore.find(prefix);
    ASSERT_FALSE(iter == thisStore.end());
    ASSERT_EQ(iter->first, prefix);
}

TEST_F(RadixStoreTest, EraseKeyThatOverlapsAnotherKeyTest) {
    std::string key = "foo";
    std::string otherKey = "foz";
    value_type value1 = std::make_pair(key, "1");
    value_type value2 = std::make_pair(otherKey, "4");
    value_type value3 = std::make_pair("bar", "5");
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));

    StringStore::size_type success = thisStore.erase(key);
    ASSERT_TRUE(success);
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    StringStore::const_iterator iter = thisStore.find(otherKey);
    ASSERT_FALSE(iter == thisStore.end());
    ASSERT_EQ(iter->first, otherKey);
}

TEST_F(RadixStoreTest, CopyTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("foz", "3");
    value_type value4 = std::make_pair("baz", "4");
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    StringStore copy(thisStore);

    std::pair<StringStore::const_iterator, bool> ins = copy.insert(value_type(value4));
    StringStore::const_iterator find1 = copy.find(value4.first);
    ASSERT_EQ(&*find1, &*ins.first);

    StringStore::const_iterator find2 = thisStore.find(value4.first);
    ASSERT_TRUE(find2 == thisStore.end());

    StringStore::const_iterator iter = thisStore.begin();
    StringStore::const_iterator copy_iter = copy.begin();

    // Both 'iter' and 'copy_iter' should point to 'bar'.
    ASSERT_EQ(iter->first, value2.first);
    ASSERT_EQ(copy_iter->first, value2.first);

    // The insertion of 'baz' split the 'bar' node in 'copy_iter', so these
    // nodes should be different.
    ASSERT_NOT_EQUALS(&*iter, &*copy_iter);

    iter++;
    copy_iter++;

    ASSERT_TRUE(copy_iter->first == "baz");

    // Node 'baz' should not be in 'thisStore'
    ASSERT_FALSE(iter->first == "baz");
    copy_iter++;

    ASSERT_EQ(&*iter, &*copy_iter);

    iter++;
    copy_iter++;
    ASSERT_EQ(&*iter, &*copy_iter);

    iter++;
    copy_iter++;
    ASSERT_TRUE(iter == thisStore.end());
    ASSERT_TRUE(copy_iter == copy.end());
}

TEST_F(RadixStoreTest, EmptyTest) {
    value_type value1 = std::make_pair("1", "foo");
    ASSERT_TRUE(thisStore.empty());

    thisStore.insert(value_type(value1));
    ASSERT_FALSE(thisStore.empty());
}

TEST_F(RadixStoreTest, NumElementsTest) {
    value_type value1 = std::make_pair("1", "foo");
    auto expected1 = StringStore::size_type(0);
    ASSERT_EQ(thisStore.size(), expected1);

    thisStore.insert(value_type(value1));
    auto expected2 = StringStore::size_type(1);
    ASSERT_EQ(thisStore.size(), expected2);
}

TEST_F(RadixStoreTest, SimpleStoreEqualityTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");

    otherStore.insert(value_type(value1));
    otherStore.insert(value_type(value2));
    otherStore.insert(value_type(value3));

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    ASSERT_TRUE(otherStore == thisStore);
}

TEST_F(RadixStoreTest, ClearTest) {
    value_type value1 = std::make_pair("1", "foo");

    thisStore.insert(value_type(value1));
    ASSERT_FALSE(thisStore.empty());

    thisStore.clear();
    ASSERT_TRUE(thisStore.empty());
}

TEST_F(RadixStoreTest, DataSizeTest) {
    std::string str1 = "foo";
    std::string str2 = "bar65";

    value_type value1 = std::make_pair("1", str1);
    value_type value2 = std::make_pair("2", str2);
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    ASSERT_EQ(thisStore.dataSize(), str1.size() + str2.size());
}

TEST_F(RadixStoreTest, DistanceTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("faz", "3");
    value_type value4 = std::make_pair("baz", "4");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    StringStore::const_iterator begin = thisStore.begin();
    StringStore::const_iterator second = thisStore.begin();
    ++second;
    StringStore::const_iterator end = thisStore.end();

    ASSERT_EQ(thisStore.distance(begin, end), 4);
    ASSERT_EQ(thisStore.distance(second, end), 3);
}

TEST_F(RadixStoreTest, MergeNoModifications) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    thisStore = baseStore;
    otherStore = baseStore;

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(expected.size(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(2));

    ASSERT_EQ(expected.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(2));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 2);
}

TEST_F(RadixStoreTest, MergeNoModificationsSharedKeyPrefix) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("food", "2");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    thisStore = baseStore;
    otherStore = baseStore;

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(expected.size(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(2));

    ASSERT_EQ(expected.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(2));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 2);
}

TEST_F(RadixStoreTest, MergeModifications) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("foo", "1234");

    value_type value3 = std::make_pair("bar", "1");
    value_type value4 = std::make_pair("bar", "1234");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value3));

    thisStore = baseStore;
    otherStore = baseStore;

    ASSERT_EQ(baseStore.size(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_EQ(otherStore.size(), StringStore::size_type(2));

    thisStore.update(value_type(value2));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(5));

    otherStore.update(value_type(value4));
    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(5));

    expected.insert(value_type(value2));
    expected.insert(value_type(value4));
    ASSERT_EQ(expected.dataSize(), StringStore::size_type(8));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(expected.dataSize(), StringStore::size_type(8));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(8));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(2));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 2);
}

TEST_F(RadixStoreTest, MergeDeletions) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("moo", "2");
    value_type value3 = std::make_pair("bar", "3");
    value_type value4 = std::make_pair("baz", "4");
    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.erase(value2.first);
    otherStore.erase(value4.first);

    expected.insert(value_type(value1));
    expected.insert(value_type(value3));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(expected.size(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(4));

    ASSERT_EQ(expected.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(4));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 2);
}

TEST_F(RadixStoreTest, MergeInsertions) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("moo", "2");
    value_type value3 = std::make_pair("bar", "3");
    value_type value4 = std::make_pair("cat", "4");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.insert(value_type(value4));
    otherStore.insert(value_type(value3));

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));
    expected.insert(value_type(value3));
    expected.insert(value_type(value4));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(expected.size(), StringStore::size_type(4));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(4));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(2));

    ASSERT_EQ(expected.dataSize(), StringStore::size_type(4));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(4));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(2));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 4);
}

TEST_F(RadixStoreTest, MergeConflictingPathCompressedKeys) {
    // This test creates a "simple" merge problem where 'otherStore' has an insertion, and
    // 'thisStore' has a non-conflicting insertion. However, due to the path compression, the trees
    // end up looking slightly different and present a challenging case.
    value_type value1 = std::make_pair("fod", "1");
    value_type value2 = std::make_pair("foda", "2");
    value_type value3 = std::make_pair("fol", "3");

    baseStore.insert(value_type(value1));
    thisStore = baseStore;
    otherStore = baseStore;

    otherStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));
    expected.insert(value_type(value3));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(otherStore.size(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(3));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(1));

    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(3));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(1));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 3);
}

TEST_F(RadixStoreTest, MergeConflictingPathCompressedKeys2) {
    // This test is similar to the one above with slight different looking trees.
    value_type value1 = std::make_pair("foe", "1");
    value_type value2 = std::make_pair("fod", "2");
    value_type value3 = std::make_pair("fol", "3");

    baseStore.insert(value_type(value1));
    thisStore = baseStore;
    otherStore = baseStore;

    otherStore.insert(value_type(value2));
    otherStore.erase(value1.first);

    thisStore.insert(value_type(value3));

    expected.insert(value_type(value2));
    expected.insert(value_type(value3));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(otherStore.size(), StringStore::size_type(1));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(1));
    ASSERT_EQ(expected.size(), StringStore::size_type(2));

    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(1));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(2));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(1));
    ASSERT_EQ(expected.dataSize(), StringStore::size_type(2));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 2);
}

TEST_F(RadixStoreTest, MergeEmptyInsertionOther) {
    value_type value1 = std::make_pair("foo", "bar");

    thisStore = baseStore;
    otherStore = baseStore;

    otherStore.insert(value_type(value1));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == otherStore);

    ASSERT_EQ(otherStore.size(), StringStore::size_type(1));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(1));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(0));

    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(3));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(3));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(0));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 1);
}

TEST_F(RadixStoreTest, MergeEmptyInsertionThis) {
    value_type value1 = std::make_pair("foo", "bar");

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.insert(value_type(value1));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == thisStore);

    ASSERT_EQ(otherStore.size(), StringStore::size_type(0));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(1));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(0));

    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(0));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(3));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(0));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 1);
}

TEST_F(RadixStoreTest, MergeInsertionDeletionModification) {
    value_type value1 = std::make_pair("1", "foo");
    value_type value2 = std::make_pair("2", "baz");
    value_type value3 = std::make_pair("3", "bar");
    value_type value4 = std::make_pair("4", "faz");
    value_type value5 = std::make_pair("5", "too");
    value_type value6 = std::make_pair("6", "moo");
    value_type value7 = std::make_pair("1", "1234");
    value_type value8 = std::make_pair("2", "12345");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.update(value_type(value7));
    thisStore.erase(value4.first);
    thisStore.insert(value_type(value5));

    otherStore.update(value_type(value8));
    otherStore.erase(value3.first);
    otherStore.insert(value_type(value6));

    expected.insert(value_type(value5));
    expected.insert(value_type(value6));
    expected.insert(value_type(value7));
    expected.insert(value_type(value8));

    thisStore.merge3(baseStore, otherStore);

    ASSERT_TRUE(thisStore == expected);

    ASSERT_EQ(otherStore.size(), StringStore::size_type(4));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(4));
    ASSERT_EQ(baseStore.size(), StringStore::size_type(4));
    ASSERT_EQ(expected.size(), StringStore::size_type(4));

    ASSERT_EQ(otherStore.dataSize(), StringStore::size_type(14));
    ASSERT_EQ(thisStore.dataSize(), StringStore::size_type(15));
    ASSERT_EQ(baseStore.dataSize(), StringStore::size_type(12));
    ASSERT_EQ(expected.dataSize(), StringStore::size_type(15));

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 4);
}

TEST_F(RadixStoreTest, MergeConflictingModifications) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("foo", "3");

    baseStore.insert(value_type(value1));

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.update(value_type(value2));

    otherStore.update(value_type(value3));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, MergeConflictingModifictionOtherAndDeletionThis) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("foo", "2");

    baseStore.insert(value_type(value1));

    thisStore = baseStore;
    otherStore = baseStore;
    thisStore.erase(value1.first);
    otherStore.update(value_type(value2));
    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, MergeConflictingModifictionThisAndDeletionOther) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("foo", "2");

    baseStore.insert(value_type(value1));

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.update(value_type(value2));

    otherStore.erase(value1.first);

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, MergeConflictingInsertions) {
    value_type value1 = std::make_pair("foo", "bar");
    value_type value2 = std::make_pair("foo", "bar");

    thisStore = baseStore;
    otherStore = baseStore;

    thisStore.insert(value_type(value2));

    otherStore.insert(value_type(value1));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, UpperBoundTest) {
    value_type value1 = std::make_pair("foo", "1");
    value_type value2 = std::make_pair("bar", "2");
    value_type value3 = std::make_pair("baz", "3");
    value_type value4 = std::make_pair("fools", "4");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value4));

    StringStore::const_iterator iter = thisStore.upper_bound(value2.first);
    ASSERT_EQ(iter->first, "baz");

    iter++;
    ASSERT_EQ(iter->first, "foo");

    iter++;
    ASSERT_EQ(iter->first, "fools");

    iter++;
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.upper_bound(value4.first);
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.upper_bound("baa");
    ASSERT_EQ(iter->first, "bar");
}

TEST_F(RadixStoreTest, LowerBoundTest) {
    value_type value1 = std::make_pair("baa", "1");
    value_type value2 = std::make_pair("bad", "2");
    value_type value3 = std::make_pair("foo", "3");
    value_type value4 = std::make_pair("fools", "4");
    value_type value5 = std::make_pair("baz", "5");

    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value4));

    StringStore::const_iterator iter = thisStore.lower_bound(value1.first);
    ASSERT_EQ(iter->first, value1.first);

    ++iter;
    ASSERT_EQ(iter->first, value2.first);

    iter = thisStore.lower_bound("bac");
    ASSERT_EQ(iter->first, "bad");

    iter++;
    ASSERT_EQ(iter->first, "foo");

    iter++;
    ASSERT_EQ(iter->first, "fools");

    iter++;
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.lower_bound("baz");
    ASSERT_TRUE(iter == thisStore.find("foo"));

    // Lower bound not found
    iter = thisStore.lower_bound("fooze");
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.lower_bound("fright");
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.lower_bound("three");
    ASSERT_TRUE(iter == thisStore.end());

    thisStore.insert(value_type(value5));
    iter = thisStore.lower_bound("bah");
    ASSERT_TRUE(iter == thisStore.find("baz"));
}

TEST_F(RadixStoreTest, LowerBoundTestSmallerThanExistingPrefix) {
    value_type value1 = std::make_pair("abcdef", "1");
    value_type value2 = std::make_pair("abc", "1");
    value_type value3 = std::make_pair("bah", "2");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    // Test the various ways in which the key we are trying to lower
    // bound can be smaller than existing keys it shares prefixes with.

    // Search key is smaller than existing key.
    StringStore::const_iterator iter = thisStore.lower_bound("abcd");
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value1.first);

    // Smaller character at mismatch between search key and existing key.
    StringStore::const_iterator iter2 = thisStore.lower_bound("abcda");
    ASSERT_TRUE(iter2 != thisStore.end());
    ASSERT_EQ(iter2->first, value1.first);
}

TEST_F(RadixStoreTest, LowerBoundTestLargerThanExistingPrefix) {
    value_type value1 = std::make_pair("abcdef", "1");
    value_type value2 = std::make_pair("abc", "1");
    value_type value3 = std::make_pair("agh", "1");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.insert(value_type(value3));

    // Test the various ways in which the key we are trying to lower
    // bound can be smaller than existing keys it shares prefixes with.

    // Search key is longer than existing key.
    StringStore::const_iterator iter = thisStore.lower_bound("abcdefg");
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value3.first);

    // Larger character at mismatch between search key and existing key.
    StringStore::const_iterator iter2 = thisStore.lower_bound("abcdz");
    ASSERT_TRUE(iter2 != thisStore.end());
    ASSERT_EQ(iter2->first, value3.first);
}

TEST_F(RadixStoreTest, LowerBoundTestExactPrefixMatch) {
    value_type value1 = std::make_pair("aba", "1");
    value_type value2 = std::make_pair("abd", "1");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    // Search for a string that matches a prefix in the tree with no value.
    StringStore::const_iterator iter = thisStore.lower_bound("ab");
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value1.first);
}

TEST_F(RadixStoreTest, LowerBoundTestNullCharacter) {
    value_type value1 = std::make_pair(std::string("ab\0", 3), "1");
    value_type value2 = std::make_pair("abd", "1");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_iterator iter = thisStore.lower_bound(std::string("ab"));
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value1.first);
}

TEST_F(RadixStoreTest, BasicInsertFindDeleteNullCharacter) {
    value_type value1 = std::make_pair(std::string("ab\0", 3), "1");
    value_type value2 = std::make_pair("abd", "1");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    StringStore::const_iterator iter = thisStore.find(std::string("ab\0", 3));
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value1.first);

    ASSERT_TRUE(thisStore.erase(std::string("ab\0", 3)));
    ASSERT_EQ(thisStore.size(), StringStore::size_type(1));

    iter = thisStore.find(std::string("ab\0", 3));
    ASSERT_TRUE(iter == thisStore.end());

    iter = thisStore.find(std::string("abd"));
    ASSERT_TRUE(iter != thisStore.end());
    ASSERT_EQ(iter->first, value2.first);
}

TEST_F(RadixStoreTest, ReverseIteratorTest) {
    value_type value1 = std::make_pair("foo", "3");
    value_type value2 = std::make_pair("bar", "1");
    value_type value3 = std::make_pair("baz", "2");
    value_type value4 = std::make_pair("fools", "5");
    value_type value5 = std::make_pair("foods", "4");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 5;
    for (auto iter = thisStore.rbegin(); iter != thisStore.rend(); ++iter) {
        ASSERT_EQ(iter->second, std::to_string(cur));
        --cur;
    }
    ASSERT_EQ(cur, 0);
}

TEST_F(RadixStoreTest, ReverseIteratorFromForwardIteratorTest) {
    value_type value1 = std::make_pair("foo", "3");
    value_type value2 = std::make_pair("bar", "1");
    value_type value3 = std::make_pair("baz", "2");
    value_type value4 = std::make_pair("fools", "5");
    value_type value5 = std::make_pair("foods", "4");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 1;
    auto iter = thisStore.begin();
    while (iter != thisStore.end()) {
        ASSERT_EQ(iter->second, std::to_string(cur));
        ++cur;
        ++iter;
    }

    ASSERT_EQ(cur, 6);
    ASSERT_TRUE(iter == thisStore.end());

    --cur;
    // This should create a reverse iterator that starts at the very beginning (for the reverse
    // iterator).
    StringStore::const_reverse_iterator riter(iter);
    while (riter != thisStore.rend()) {
        ASSERT_EQ(riter->second, std::to_string(cur));
        --cur;
        ++riter;
    }

    ASSERT_EQ(cur, 0);
}

TEST_F(RadixStoreTest, ReverseIteratorFromMiddleOfForwardIteratorTest) {
    value_type value1 = std::make_pair("foo", "3");
    value_type value2 = std::make_pair("bar", "1");
    value_type value3 = std::make_pair("baz", "2");
    value_type value4 = std::make_pair("fools", "5");
    value_type value5 = std::make_pair("foods", "4");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 3;
    auto iter = thisStore.begin();
    ++iter;
    ++iter;
    ++iter;

    // This should create a reverse iterator that starts at the node 'foo'
    StringStore::const_reverse_iterator riter(iter);
    while (riter != thisStore.rend()) {
        ASSERT_EQ(riter->second, std::to_string(cur));
        --cur;
        ++riter;
    }

    ASSERT_EQ(cur, 0);
}

TEST_F(RadixStoreTest, ReverseIteratorCopyConstructorTest) {
    value_type value1 = std::make_pair("foo", "3");
    value_type value2 = std::make_pair("bar", "1");
    value_type value3 = std::make_pair("baz", "2");
    value_type value4 = std::make_pair("fools", "5");
    value_type value5 = std::make_pair("foods", "4");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 3;
    auto iter = thisStore.begin();
    auto iter2(iter);
    ASSERT_EQ(&*iter, &*iter2);

    ++iter;
    ++iter2;
    ASSERT_EQ(&*iter, &*iter2);

    ++iter;
    ++iter2;
    ASSERT_EQ(&*iter, &*iter2);

    ++iter;
    ++iter2;
    ASSERT_EQ(&*iter, &*iter2);

    // This should create a reverse iterator that starts at the node 'foo'
    StringStore::const_reverse_iterator riter(iter);
    StringStore::const_reverse_iterator riter2(riter);
    while (riter != thisStore.rend()) {
        ASSERT_EQ(&*riter, &*riter2);
        --cur;
        ++riter;
        ++riter2;
    }

    ASSERT_EQ(cur, 0);
}

TEST_F(RadixStoreTest, ReverseIteratorAssignmentOpTest) {
    value_type value1 = std::make_pair("foo", "3");
    value_type value2 = std::make_pair("bar", "1");
    value_type value3 = std::make_pair("baz", "2");
    value_type value4 = std::make_pair("fools", "5");
    value_type value5 = std::make_pair("foods", "4");

    thisStore.insert(value_type(value4));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value3));
    thisStore.insert(value_type(value2));

    int cur = 5;
    auto iter = thisStore.begin();

    StringStore::const_reverse_iterator riter(iter);
    riter = thisStore.rbegin();
    while (riter != thisStore.rend()) {
        ASSERT_EQ(riter->second, std::to_string(cur));
        --cur;
        ++riter;
    }

    ASSERT_EQ(cur, 0);
}

TEST_F(RadixStoreTest, PathCompressionTest) {
    value_type value1 = std::make_pair("food", "1");
    value_type value2 = std::make_pair("foo", "2");
    value_type value3 = std::make_pair("bar", "3");
    value_type value4 = std::make_pair("batter", "4");
    value_type value5 = std::make_pair("batty", "5");
    value_type value6 = std::make_pair("bats", "6");
    value_type value7 = std::make_pair("foodie", "7");

    thisStore.insert(value_type(value1));
    ASSERT_EQ(thisStore.to_string_for_test(), "\n food*\n");

    // Add a key that is a prefix of a key already in the tree
    thisStore.insert(value_type(value2));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n foo*"
              "\n  d*\n");

    // Add a key with no prefix already in the tree
    thisStore.insert(value_type(value3));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n bar*"
              "\n foo*"
              "\n  d*\n");

    // Add a key that shares a prefix with a key in the tree
    thisStore.insert(value_type(value4));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n ba"
              "\n  r*"
              "\n  tter*"
              "\n foo*"
              "\n  d*\n");

    // Add another level to the tree
    thisStore.insert(value_type(value5));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n ba"
              "\n  r*"
              "\n  tt"
              "\n   er*"
              "\n   y*"
              "\n foo*"
              "\n  d*\n");

    // Erase a key that causes the path to be compressed
    thisStore.erase(value2.first);
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n ba"
              "\n  r*"
              "\n  tt"
              "\n   er*"
              "\n   y*"
              "\n food*\n");

    // Erase a key that causes the path to be compressed
    thisStore.erase(value3.first);
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n batt"
              "\n  er*"
              "\n  y*"
              "\n food*\n");

    // Add a key that causes a node with children to be split
    thisStore.insert(value_type(value6));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n bat"
              "\n  s*"
              "\n  t"
              "\n   er*"
              "\n   y*"
              "\n food*\n");

    // Add a key that has a prefix already in the tree with a value
    thisStore.insert(value_type(value7));
    ASSERT_EQ(thisStore.to_string_for_test(),
              "\n bat"
              "\n  s*"
              "\n  t"
              "\n   er*"
              "\n   y*"
              "\n food*"
              "\n  ie*\n");
}

TEST_F(RadixStoreTest, MergeOneTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<collection-2-first", "1");
    value_type value5 = std::make_pair("<collection-2-second", "2");
    value_type value6 = std::make_pair("<collection-1-second", "20");
    value_type value7 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value2));
    baseStore.insert(value_type(value3));

    otherStore = baseStore;
    thisStore = baseStore;
    parallelStore = baseStore;

    thisStore.update(value_type(value6));
    thisStore.update(value_type(value7));

    parallelStore.insert(value_type(value5));
    parallelStore.insert(value_type(value4));

    parallelStore.merge3(baseStore, otherStore);
    thisStore.merge3(baseStore, parallelStore);

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 5);
}

TEST_F(RadixStoreTest, MergeTwoTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<_index", "index");
    value_type value5 = std::make_pair("<collection-1-third", "1");
    value_type value6 = std::make_pair("<collection-1-forth", "2");
    value_type value7 = std::make_pair("<collection-1-first", "20");
    value_type value8 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    otherStore = baseStore;
    thisStore = baseStore;
    parallelStore = baseStore;

    parallelStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    parallelStore.merge3(baseStore, otherStore);
    thisStore.merge3(baseStore, parallelStore);

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 4);
}

TEST_F(RadixStoreTest, MergeThreeTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<_index", "index");
    value_type value5 = std::make_pair("<collection-1-third", "1");
    value_type value6 = std::make_pair("<collection-1-forth", "2");
    value_type value7 = std::make_pair("<collection-1-first", "20");
    value_type value8 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    otherStore = baseStore;
    thisStore = baseStore;
    parallelStore = baseStore;

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));
    thisStore.update(value_type(value7));
    thisStore.update(value_type(value8));
    thisStore.insert(value_type(value5));
    thisStore.insert(value_type(value6));

    thisStore.merge3(baseStore, otherStore);

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 6);
}

TEST_F(RadixStoreTest, MergeFourTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<_index", "index");
    value_type value5 = std::make_pair("<collection-1-third", "1");
    value_type value6 = std::make_pair("<collection-1-forth", "2");
    value_type value7 = std::make_pair("<collection-1-first", "20");
    value_type value8 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    otherStore = baseStore;
    thisStore = baseStore;

    otherStore.insert(value_type(value2));
    otherStore.update(value_type(value7));

    thisStore.merge3(baseStore, otherStore);

    int itemsVisited = 0;
    StringStore::const_iterator thisIter = thisStore.begin();
    while (thisIter != thisStore.end()) {
        itemsVisited++;
        thisIter++;
    }

    ASSERT_EQ(itemsVisited, 4);
}

TEST_F(RadixStoreTest, MergeFiveTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<_index", "index");
    value_type value5 = std::make_pair("<collection-1-third", "1");
    value_type value6 = std::make_pair("<collection-1-forth", "2");
    value_type value7 = std::make_pair("<collection-1-first", "20");
    value_type value8 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    otherStore = baseStore;
    thisStore = baseStore;

    otherStore.update(value_type(value7));
    thisStore.update(value_type(value7));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, MergeSixTest) {
    value_type value1 = std::make_pair("<collection-1-first", "1");
    value_type value2 = std::make_pair("<collection-1-second", "2");
    value_type value3 = std::make_pair("<_catalog", "catalog");
    value_type value4 = std::make_pair("<_index", "index");
    value_type value5 = std::make_pair("<collection-1-third", "1");
    value_type value6 = std::make_pair("<collection-1-forth", "2");
    value_type value7 = std::make_pair("<collection-1-first", "20");
    value_type value8 = std::make_pair("<collection-1-second", "30");

    baseStore.insert(value_type(value1));
    baseStore.insert(value_type(value3));
    baseStore.insert(value_type(value4));

    otherStore = baseStore;
    thisStore = baseStore;

    otherStore.update(value_type(value7));
    otherStore.insert(value_type(value2));

    thisStore.update(value_type(value7));

    ASSERT_THROWS(thisStore.merge3(baseStore, otherStore), merge_conflict_exception);
}

TEST_F(RadixStoreTest, MergeSevenTest) {
    value_type value1 = std::make_pair("<collection-1", "1");
    value_type value2 = std::make_pair("<collection-2", "2");

    baseStore.insert(value_type(value1));

    otherStore = baseStore;
    thisStore = baseStore;

    otherStore.insert(value_type(value2));

    thisStore.merge3(baseStore, otherStore);

    expected.insert(value_type(value1));
    expected.insert(value_type(value2));

    ASSERT_TRUE(thisStore == expected);
    ASSERT_TRUE(thisStore.size() == 2);
    ASSERT_TRUE(thisStore.dataSize() == 2);
}

TEST_F(RadixStoreTest, SizeTest) {
    value_type value1 = std::make_pair("<index", ".");
    value_type value2 = std::make_pair("<collection", "..");
    value_type value3 = std::make_pair("<collection-1", "...");
    value_type value4 = std::make_pair("<collection-2", "....");

    thisStore.insert(value_type(value1));
    ASSERT_TRUE(thisStore.size() == 1);
    ASSERT_TRUE(thisStore.dataSize() == 1);

    thisStore.insert(value_type(value2));
    ASSERT_TRUE(thisStore.size() == 2);
    ASSERT_TRUE(thisStore.dataSize() == 3);

    thisStore.insert(value_type(value3));
    ASSERT_TRUE(thisStore.size() == 3);
    ASSERT_TRUE(thisStore.dataSize() == 6);

    thisStore.insert(value_type(value4));
    ASSERT_TRUE(thisStore.size() == 4);
    ASSERT_TRUE(thisStore.dataSize() == 10);

    thisStore.erase(value2.first);
    ASSERT_TRUE(thisStore.size() == 3);
    ASSERT_TRUE(thisStore.dataSize() == 8);

    thisStore.erase(value4.first);
    ASSERT_TRUE(thisStore.size() == 2);
    ASSERT_TRUE(thisStore.dataSize() == 4);

    thisStore.erase(value1.first);
    ASSERT_TRUE(thisStore.size() == 1);
    ASSERT_TRUE(thisStore.dataSize() == 3);

    thisStore.erase(value3.first);
    ASSERT_TRUE(thisStore.size() == 0);
    ASSERT_TRUE(thisStore.dataSize() == 0);

    thisStore.insert(value_type(value4));
    ASSERT_TRUE(thisStore.size() == 1);
    ASSERT_TRUE(thisStore.dataSize() == 4);

    thisStore.insert(value_type(value3));
    ASSERT_TRUE(thisStore.size() == 2);
    ASSERT_TRUE(thisStore.dataSize() == 7);

    thisStore.insert(value_type(value2));
    ASSERT_TRUE(thisStore.size() == 3);
    ASSERT_TRUE(thisStore.dataSize() == 9);

    thisStore.insert(value_type(value1));
    ASSERT_TRUE(thisStore.size() == 4);
    ASSERT_TRUE(thisStore.dataSize() == 10);
}

TEST_F(RadixStoreTest, CannotRevalidateExhaustedCursor) {
    value_type value1 = std::make_pair("a", "1");
    value_type value2 = std::make_pair("b", "2");

    thisStore.insert(value_type(value1));

    auto it = thisStore.begin();
    it++;

    // 'it' should be exhausted.
    ASSERT_TRUE(it == thisStore.end());

    thisStore.insert(value_type(value2));

    // 'it' should still be exhausted even though we have a new tree version available.
    ASSERT_TRUE(it == thisStore.end());
}

TEST_F(RadixStoreTest, AvoidComparingDifferentTreeVersions) {
    value_type value = std::make_pair("a", "1");
    value_type value2 = std::make_pair("b", "2");
    value_type updated = std::make_pair("a", "10");

    thisStore.insert(value_type(value));
    thisStore.insert(value_type(value2));

    {
        auto it = thisStore.begin();

        // Updating value1 causes a new tree to be made since it's shared with the cursor.
        thisStore.update(value_type(updated));

        auto it2 = thisStore.begin();

        it.repositionIfChanged();
        ASSERT_TRUE(it2 == it);
    }

    {
        auto it = thisStore.begin();

        // Updating value1 causes a new tree to be made since it's shared with the cursor.
        thisStore.erase("a");

        auto it2 = thisStore.begin();

        it.repositionIfChanged();
        ASSERT_TRUE(it2->first == "b");
        ASSERT_TRUE(it2 == it);
    }
}

TEST_F(RadixStoreTest, TreeUniqueness) {
    value_type value1 = std::make_pair("a", "1");
    value_type value2 = std::make_pair("b", "2");
    value_type value3 = std::make_pair("c", "3");
    value_type value4 = std::make_pair("d", "4");

    auto rootAddr = getRootAddress();
    thisStore.insert(value_type(value1));

    // Neither the address or count should change.
    ASSERT_EQUALS(rootAddr, getRootAddress());
    ASSERT_EQUALS(1, getRootCount());

    thisStore.insert(value_type(value2));
    ASSERT_EQUALS(rootAddr, getRootAddress());
    ASSERT_EQUALS(1, getRootCount());

    {
        // Make the tree shared.
        auto it = thisStore.begin();
        ASSERT_EQUALS(rootAddr, getRootAddress());
        ASSERT_EQUALS(2, getRootCount());

        // Inserting should make a copy of the tree.
        thisStore.insert(value_type(value3));

        // The root's address should change.
        ASSERT_NOT_EQUALS(rootAddr, getRootAddress());
        rootAddr = getRootAddress();

        // Count should remain 2 because of _nextVersion
        ASSERT_EQUALS(2, getRootCount());

        // Inserting again shouldn't make a copy because the cursor hasn't been updated
        thisStore.insert(value_type(value4));
        ASSERT_EQUALS(rootAddr, getRootAddress());
        ASSERT_EQUALS(2, getRootCount());

        // Use the pointer to reposition it on the new tree.
        *it;
        ASSERT_EQUALS(rootAddr, getRootAddress());
        ASSERT_EQUALS(2, getRootCount());

        thisStore.erase("d");
        ASSERT_NOT_EQUALS(rootAddr, getRootAddress());
        rootAddr = getRootAddress();
        ASSERT_EQUALS(2, getRootCount());
    }

    ASSERT_EQUALS(rootAddr, getRootAddress());
    ASSERT_EQUALS(1, getRootCount());

    thisStore.erase("c");
    thisStore.erase("b");
    thisStore.erase("a");

    ASSERT_EQUALS(rootAddr, getRootAddress());
    ASSERT_EQUALS(1, getRootCount());
}

TEST_F(RadixStoreTest, HasPreviousVersionFlagTest) {
    value_type value1 = std::make_pair("a", "1");
    value_type value2 = std::make_pair("b", "2");
    value_type value3 = std::make_pair("c", "3");

    ASSERT_FALSE(hasPreviousVersion());
    thisStore.insert(value_type(value1));

    {
        auto it = thisStore.begin();
        ASSERT_FALSE(hasPreviousVersion());
    }

    ASSERT_FALSE(hasPreviousVersion());

    {
        auto it = thisStore.begin();
        ASSERT_FALSE(hasPreviousVersion());

        thisStore.insert(value_type(value2));
        ASSERT_TRUE(hasPreviousVersion());
    }

    ASSERT_FALSE(hasPreviousVersion());
    thisStore.erase("b");

    // Use multiple cursors
    {
        auto it = thisStore.begin();
        auto it2 = thisStore.begin();
        ASSERT_FALSE(hasPreviousVersion());

        thisStore.insert(value_type(value2));
        ASSERT_TRUE(hasPreviousVersion());

        *it;  // Change to repositionIfChanged when merging (SERVER-38262 in master);
        ASSERT_TRUE(hasPreviousVersion());

        *it2;
        ASSERT_FALSE(hasPreviousVersion());

        thisStore.insert(value_type(value3));
        ASSERT_TRUE(hasPreviousVersion());

        *it;
    }

    ASSERT_FALSE(hasPreviousVersion());
}

TEST_F(RadixStoreTest, LowerBoundEndpoint) {
    value_type value1 = std::make_pair("AAA", "1");
    value_type value2 = std::make_pair("\xff\xff\xff", "2");

    thisStore.insert(value_type(value1));
    thisStore.insert(value_type(value2));

    auto it = thisStore.lower_bound("AAA");
    ASSERT_TRUE(it->first == "AAA");

    it = thisStore.lower_bound("\xff\xff");
    ASSERT_TRUE(it->first == "\xff\xff\xff");

    it = thisStore.lower_bound("\xff\xff\xff");
    ASSERT_TRUE(it->first == "\xff\xff\xff");

    it = thisStore.lower_bound("\xff\xff\xff\xff");
    ASSERT_TRUE(it == thisStore.end());
}

}  // biggie namespace
}  // mongo namespace
