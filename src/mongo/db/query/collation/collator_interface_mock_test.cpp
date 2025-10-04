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

#include "mongo/db/query/collation/collator_interface_mock.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"


namespace {

using namespace mongo;

TEST(CollatorInterfaceMockSelfTest, MocksOfSameTypeAreEqual) {
    CollatorInterfaceMock reverseMock1(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock reverseMock2(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT(reverseMock1 == reverseMock2);

    CollatorInterfaceMock alwaysEqualMock1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock alwaysEqualMock2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ASSERT(alwaysEqualMock1 == alwaysEqualMock2);

    CollatorInterfaceMock toLowerMock1(CollatorInterfaceMock::MockType::kToLowerString);
    CollatorInterfaceMock toLowerMock2(CollatorInterfaceMock::MockType::kToLowerString);
    ASSERT(toLowerMock1 == toLowerMock2);
}

TEST(CollatorInterfaceMockSelfTest, MocksOfDifferentTypesAreNotEqual) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualMock(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ASSERT(reverseMock != alwaysEqualMock);
}

TEST(CollatorInterfaceMockSelfTest, NullMockPointersMatch) {
    ASSERT(CollatorInterface::collatorsMatch(nullptr, nullptr));
}

TEST(CollatorInterfaceMockSelfTest, NullMockPointerDoesNotMatchNonNullMockPointer) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT(!CollatorInterface::collatorsMatch(nullptr, &reverseMock));
    ASSERT(!CollatorInterface::collatorsMatch(&reverseMock, nullptr));
}

TEST(CollatorInterfaceMockSelfTest, PointersToMocksOfSameTypeMatch) {
    CollatorInterfaceMock reverseMock1(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock reverseMock2(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT(CollatorInterface::collatorsMatch(&reverseMock1, &reverseMock2));
}

TEST(CollatorInterfaceMockSelfTest, PointersToMocksOfDifferentTypesDoNotMatch) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualMock(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ASSERT(!CollatorInterface::collatorsMatch(&reverseMock, &alwaysEqualMock));
}

TEST(CollatorInterfaceMockSelfTest, ClonedMockMatchesOriginal) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    auto reverseClone = reverseMock.clone();
    ASSERT(CollatorInterface::collatorsMatch(reverseClone.get(), &reverseMock));

    CollatorInterfaceMock alwaysEqualMock(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto alwaysEqualClone = alwaysEqualMock.clone();
    ASSERT(CollatorInterface::collatorsMatch(alwaysEqualClone.get(), &alwaysEqualMock));

    CollatorInterfaceMock toLowerMock(CollatorInterfaceMock::MockType::kToLowerString);
    auto toLowerClone = toLowerMock.clone();
    ASSERT(CollatorInterface::collatorsMatch(toLowerClone.get(), &toLowerMock));
}

TEST(CollatorInterfaceMockSelfTest, ReverseMockComparesInReverse) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_EQ(reverseMock.compare("abc", "abc"), 0);
    ASSERT_GT(reverseMock.compare("abc", "cba"), 0);
    ASSERT_LT(reverseMock.compare("cba", "abc"), 0);
}

TEST(CollatorInterfaceMockSelfTest, ReverseMockComparisonKeysCompareInReverse) {
    CollatorInterfaceMock reverseMock(CollatorInterfaceMock::MockType::kReverseString);
    auto keyABC = reverseMock.getComparisonKey("abc");
    auto keyCBA = reverseMock.getComparisonKey("cba");
    ASSERT_EQ(keyABC.getKeyData().compare(keyABC.getKeyData()), 0);
    ASSERT_GT(keyABC.getKeyData().compare(keyCBA.getKeyData()), 0);
    ASSERT_LT(keyCBA.getKeyData().compare(keyABC.getKeyData()), 0);
}

TEST(CollatorInterfaceMockSelfTest, AlwaysEqualMockAlwaysComparesEqual) {
    CollatorInterfaceMock alwaysEqualMock(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ASSERT_EQ(alwaysEqualMock.compare("abc", "efg"), 0);
    ASSERT_EQ(alwaysEqualMock.compare("efg", "abc"), 0);
    ASSERT_EQ(alwaysEqualMock.compare("abc", "abc"), 0);
}

TEST(CollatorInterfaceMockSelfTest, AlwaysEqualMockComparisonKeysAlwaysCompareEqual) {
    CollatorInterfaceMock alwaysEqualMock(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto keyABC = alwaysEqualMock.getComparisonKey("abc");
    auto keyEFG = alwaysEqualMock.getComparisonKey("efg");
    ASSERT_EQ(keyABC.getKeyData().compare(keyEFG.getKeyData()), 0);
    ASSERT_EQ(keyEFG.getKeyData().compare(keyABC.getKeyData()), 0);
    ASSERT_EQ(keyABC.getKeyData().compare(keyABC.getKeyData()), 0);
}

TEST(CollatorInterfaceMockSelfTest, ToLowerMockComparesInLowerCase) {
    CollatorInterfaceMock toLowerMock(CollatorInterfaceMock::MockType::kToLowerString);
    ASSERT_EQ(toLowerMock.compare("foo", "FOO"), 0);
    ASSERT_EQ(toLowerMock.compare("bar", "BAR"), 0);
    ASSERT_GT(toLowerMock.compare("bar", "ABC"), 0);
}

TEST(CollatorInterfaceMockSelfTest, ToLowerMockComparisonKeysCompareInLowerCase) {
    CollatorInterfaceMock toLowerMock(CollatorInterfaceMock::MockType::kToLowerString);
    auto keyFOO = toLowerMock.getComparisonKey("FOO");
    auto keyFoo = toLowerMock.getComparisonKey("foo");
    ASSERT_EQ(keyFOO.getKeyData().compare(keyFoo.getKeyData()), 0);
}

TEST(CollatorInterfaceMockSelfTest, WoCompareStringsWithMockCollator) {
    BSONObj left = BSON("a" << "a"
                            << "b"
                            << "xyz"
                            << "c"
                            << "c");
    BSONObj right = BSON("a" << "a"
                             << "b"
                             << "zyx"
                             << "c"
                             << "c");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_GT(left.woCompare(right, BSONObj(), true, &collator), 0);
    ASSERT_LT(right.woCompare(left, BSONObj(), true, &collator), 0);
}

TEST(CollatorInterfaceMockSelfTest, WoCompareNestedObjectsWithMockCollator) {
    BSONObj left = mongo::fromjson("{a: {a: 'a', b: 'xyz', c: 'c'}}");
    BSONObj right = mongo::fromjson("{a: {a: 'a', b: 'zyx', c: 'c'}}");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_GT(left.woCompare(right, BSONObj(), true, &collator), 0);
    ASSERT_LT(right.woCompare(left, BSONObj(), true, &collator), 0);
}

TEST(CollatorInterfaceMockSelfTest, WoCompareNestedArraysWithMockCollator) {
    BSONObj left = mongo::fromjson("{a: ['a', 'xyz', 'c']}");
    BSONObj right = mongo::fromjson("{a: ['a', 'zyx', 'c']}");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_GT(left.woCompare(right, BSONObj(), true, &collator), 0);
    ASSERT_LT(right.woCompare(left, BSONObj(), true, &collator), 0);
}

TEST(CollatorInterfaceMockSelfTest, WoCompareNumbersWithMockCollator) {
    BSONObj left = BSON("a" << 1);
    BSONObj right = BSON("a" << 2);
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_LT(left.woCompare(right, BSONObj(), true, &collator), 0);
    ASSERT_GT(right.woCompare(left, BSONObj(), true, &collator), 0);
}

TEST(CollatorInterfaceMockSelfTest, MockCollatorReportsMockVersionString) {
    CollatorInterfaceMock reverseCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ASSERT_EQ(*reverseCollator.getSpec().getVersion(), "mock_version");
    ASSERT_EQ(*alwaysEqualCollator.getSpec().getVersion(), "mock_version");
    ASSERT_EQ(*toLowerCollator.getSpec().getVersion(), "mock_version");
}

TEST(CollatorInterfaceMockSelfTest, StringsAreHashedWithRespectToCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    auto tryHash = [&](StringData s) {
        size_t h = 0;
        toLowerCollator.hash_combine(h, s);
        return h;
    };
    ASSERT_EQ(tryHash("foo"), tryHash("FOO"));
    ASSERT_NE(tryHash("foo"), tryHash("FOOz"));
}

TEST(CollatorInterfaceMockSelfTest, BSONObjsEqualUnderCollatorHashEqually) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    SimpleBSONObjComparator bsonCmpConsiderCase;
    BSONObjComparator bsonCmpIgnoreCase(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &toLowerCollator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "FOO");
    ASSERT_NE(bsonCmpConsiderCase.hash(obj1), bsonCmpConsiderCase.hash(obj2));
    ASSERT_EQ(bsonCmpIgnoreCase.hash(obj1), bsonCmpIgnoreCase.hash(obj2));
}

TEST(CollatorInterfaceMockSelfTest, BSONObjsEqualUnderCollatorHashEquallyNested) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    SimpleBSONObjComparator bsonCmpConsiderCase;
    BSONObjComparator bsonCmpIgnoreCase(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &toLowerCollator);
    BSONObj obj1 = BSON("a" << 1 << "b" << BSON("c" << "foo"));
    BSONObj obj2 = BSON("a" << 1 << "b" << BSON("c" << "FOO"));
    ASSERT_NE(bsonCmpConsiderCase.hash(obj1), bsonCmpConsiderCase.hash(obj2));
    ASSERT_EQ(bsonCmpIgnoreCase.hash(obj1), bsonCmpIgnoreCase.hash(obj2));
}

TEST(CollatorInterfaceMockSelfTest, BSONElementsEqualUnderCollatorHashEqually) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    SimpleBSONElementComparator bsonCmpConsiderCase;
    BSONElementComparator bsonCmpIgnoreCase(BSONElementComparator::FieldNamesMode::kConsider,
                                            &toLowerCollator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "FOO");
    BSONElement elt1 = obj1.firstElement();
    BSONElement elt2 = obj2.firstElement();
    ASSERT_NE(bsonCmpConsiderCase.hash(elt1), bsonCmpConsiderCase.hash(elt2));
    ASSERT_EQ(bsonCmpIgnoreCase.hash(elt1), bsonCmpIgnoreCase.hash(elt2));
}

TEST(CollatorInterfaceMockSelfTest, BSONElementsEqualUnderCollatorHashEquallyNested) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    SimpleBSONElementComparator bsonCmpConsiderCase;
    BSONElementComparator bsonCmpIgnoreCase(BSONElementComparator::FieldNamesMode::kConsider,
                                            &toLowerCollator);
    BSONObj obj1 = BSON("a" << BSON("b" << "foo"
                                        << "c" << BSON("d" << "BaR")));
    BSONObj obj2 = BSON("a" << BSON("b" << "FOO"
                                        << "c" << BSON("d" << "bar")));
    BSONElement elt1 = obj1.firstElement();
    BSONElement elt2 = obj2.firstElement();
    ASSERT_NE(bsonCmpConsiderCase.hash(elt1), bsonCmpConsiderCase.hash(elt2));
    ASSERT_EQ(bsonCmpIgnoreCase.hash(elt1), bsonCmpIgnoreCase.hash(elt2));
}

TEST(CollatorInterfaceMockSelfTest, CollatorGeneratedUnorderedSetOfBSONObjRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    BSONObjComparator bsonCmpIgnoreCase(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &toLowerCollator);
    auto set = bsonCmpIgnoreCase.makeBSONObjUnorderedSet();
    set.insert(BSON("a" << "foo"));
    set.insert(BSON("a" << "FOO"));
    set.insert(BSON("a" << "FOOz"));
    ASSERT_EQ(set.size(), 2U);
    ASSERT_EQ(set.count(BSON("a" << "FoO")), 1U);
    ASSERT_EQ(set.count(BSON("a" << "fooZ")), 1U);
}

TEST(CollatorInterfaceMockSelfTest, CollatorGeneratedUnorderedMapOfBSONObjRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    BSONObjComparator bsonCmpIgnoreCase(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &toLowerCollator);
    auto map = bsonCmpIgnoreCase.makeBSONObjIndexedUnorderedMap<int>();
    map[BSON("a" << "foo")] = 1;
    map[BSON("a" << "FOO")] = 2;
    map[BSON("a" << "FOOz")] = 3;
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map[BSON("a" << "FoO")], 2);
    ASSERT_EQ(map[BSON("a" << "fooZ")], 3);
}

TEST(CollatorInterfaceMockSelfTest, CollatorGeneratedUnorderedSetOfBSONElementRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    BSONElementComparator bsonCmpIgnoreCase(BSONElementComparator::FieldNamesMode::kConsider,
                                            &toLowerCollator);
    auto set = bsonCmpIgnoreCase.makeBSONEltUnorderedSet();

    BSONObj obj1 = BSON("a" << BSON("b" << "foo"));
    set.insert(obj1.firstElement());
    BSONObj obj2 = BSON("a" << BSON("b" << "FOO"));
    set.insert(obj2.firstElement());
    BSONObj obj3 = BSON("a" << BSON("b" << "FOOz"));
    set.insert(obj3.firstElement());

    ASSERT_EQ(set.size(), 2U);
    BSONObj obj4 = BSON("a" << BSON("b" << "FoO"));
    ASSERT_EQ(set.count(obj4.firstElement()), 1U);
    BSONObj obj5 = BSON("a" << BSON("b" << "fooZ"));
    ASSERT_EQ(set.count(obj5.firstElement()), 1U);
}

TEST(CollatorInterfaceMockSelfTest, CollatorGeneratedUnorderedMapOfBSONElementRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    BSONElementComparator bsonCmpIgnoreCase(BSONElementComparator::FieldNamesMode::kConsider,
                                            &toLowerCollator);
    auto map = bsonCmpIgnoreCase.makeBSONEltIndexedUnorderedMap<int>();

    BSONObj obj1 = BSON("a" << BSON("b" << "foo"));
    map[obj1.firstElement()] = 1;
    BSONObj obj2 = BSON("a" << BSON("b" << "FOO"));
    map[obj2.firstElement()] = 2;
    BSONObj obj3 = BSON("a" << BSON("b" << "FOOz"));
    map[obj3.firstElement()] = 3;

    ASSERT_EQ(map.size(), 2U);
    BSONObj obj4 = BSON("a" << BSON("b" << "FoO"));
    ASSERT_EQ(map[obj4.firstElement()], 2);
    BSONObj obj5 = BSON("a" << BSON("b" << "fooZ"));
    ASSERT_EQ(map[obj5.firstElement()], 3);
}

}  // namespace
