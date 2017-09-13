/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CollationBSONComparisonTest, CompareCodeWScopeElementWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT(comparator.evaluate(obj1["a"] != obj2["a"]));
    ASSERT_BSONELT_NE(obj1["a"], obj2["a"]);
}

TEST(CollationBSONComparisonTest, CompareCodeWScopeObjWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT(comparator.evaluate(obj1 != obj2));
    ASSERT_BSONOBJ_NE(obj1, obj2);
}

TEST(CollationBSONComparisonTest, HashingCodeWScopeElementWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT_NE(SimpleBSONElementComparator::kInstance.hash(obj1["a"]),
              SimpleBSONElementComparator::kInstance.hash(obj2["a"]));
    ASSERT_NE(comparator.hash(obj1["a"]), comparator.hash(obj2["a"]));
}

TEST(CollationBSONComparisonTest, HashingCodeWScopeObjWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code",
                                              BSON("foo"
                                                   << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT_NE(SimpleBSONObjComparator::kInstance.hash(obj1),
              SimpleBSONObjComparator::kInstance.hash(obj2));
    ASSERT_NE(comparator.hash(obj1), comparator.hash(obj2));
}

TEST(CollationBSONComparisonTest, ElementStringComparisonShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a"
                        << "not foo");
    ASSERT(comparator.evaluate(obj1["a"] == obj2["a"]));
}

TEST(CollationBSONComparisonTest, ElementStringHashShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a"
                        << "not foo");
    ASSERT_EQ(comparator.hash(obj1["a"]), comparator.hash(obj2["a"]));
}

TEST(CollationBSONComparisonTest, ObjStringComparisonShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a"
                        << "not foo");
    ASSERT(comparator.evaluate(obj1 == obj2));
}

TEST(CollationBSONComparisonTest, ObjStringHashShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a"
                        << "not foo");
    ASSERT_EQ(comparator.hash(obj1), comparator.hash(obj2));
}

TEST(CollationBSONComparisonTest, ElementCodeComparisonShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << BSONCode("foo"));
    BSONObj obj2 = BSON("a" << BSONCode("not foo"));
    ASSERT(comparator.evaluate(obj1["a"] != obj2["a"]));
}

TEST(CollationBSONComparisonTest, ElementCodeHashShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << BSONCode("foo"));
    BSONObj obj2 = BSON("a" << BSONCode("not foo"));
    ASSERT_NE(comparator.hash(obj1["a"]), comparator.hash(obj2["a"]));
}

TEST(CollationBSONComparisonTest, ObjCodeComparisonShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCode("foo"));
    BSONObj obj2 = BSON("a" << BSONCode("not foo"));
    ASSERT(comparator.evaluate(obj1 != obj2));
}

TEST(CollationBSONComparisonTest, ObjCodeHashShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCode("foo"));
    BSONObj obj2 = BSON("a" << BSONCode("not foo"));
    ASSERT_NE(comparator.hash(obj1), comparator.hash(obj2));
}

TEST(CollationBSONComparisonTest, IdenticalCodeAndStringValuesAreNotEqual) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a" << BSONCode("foo"));
    ASSERT_BSONOBJ_NE(obj1, obj2);
    ASSERT(comparator.evaluate(obj1 != obj2));
}

TEST(CollationBSONComparisonTest, IdenticalCodeAndStringValuesDoNotHashEqually) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a"
                        << "foo");
    BSONObj obj2 = BSON("a" << BSONCode("foo"));
    ASSERT_NE(comparator.hash(obj1), comparator.hash(obj2));
    ASSERT_NE(SimpleBSONObjComparator::kInstance.hash(obj1),
              SimpleBSONObjComparator::kInstance.hash(obj2));
}

}  // namespace
}  // namespace mongo
