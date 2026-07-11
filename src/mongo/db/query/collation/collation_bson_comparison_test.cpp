// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
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
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT(comparator.evaluate(obj1["a"] != obj2["a"]));
    ASSERT_BSONELT_NE(obj1["a"], obj2["a"]);
}

TEST(CollationBSONComparisonTest, CompareCodeWScopeObjWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT(comparator.evaluate(obj1 != obj2));
    ASSERT_BSONOBJ_NE(obj1, obj2);
}

TEST(CollationBSONComparisonTest, HashingCodeWScopeElementWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT_NE(SimpleBSONElementComparator::kInstance.hash(obj1["a"]),
              SimpleBSONElementComparator::kInstance.hash(obj2["a"]));
    ASSERT_NE(comparator.hash(obj1["a"]), comparator.hash(obj2["a"]));
}

TEST(CollationBSONComparisonTest, HashingCodeWScopeObjWithCollationShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "bar")));
    BSONObj obj2 = BSON("a" << BSONCodeWScope("js code", BSON("foo" << "not bar")));
    // The elements are not equal with or without the "always equal" collator.
    ASSERT_NE(SimpleBSONObjComparator::kInstance.hash(obj1),
              SimpleBSONObjComparator::kInstance.hash(obj2));
    ASSERT_NE(comparator.hash(obj1), comparator.hash(obj2));
}

TEST(CollationBSONComparisonTest, ElementStringComparisonShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "not foo");
    ASSERT(comparator.evaluate(obj1["a"] == obj2["a"]));
}

TEST(CollationBSONComparisonTest, ElementStringHashShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONElementComparator comparator(BSONElementComparator::FieldNamesMode::kConsider,
                                           &collator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "not foo");
    ASSERT_EQ(comparator.hash(obj1["a"]), comparator.hash(obj2["a"]));
}

TEST(CollationBSONComparisonTest, ObjStringComparisonShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "not foo");
    ASSERT(comparator.evaluate(obj1 == obj2));
}

TEST(CollationBSONComparisonTest, ObjStringHashShouldRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << "not foo");
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
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << BSONCode("foo"));
    ASSERT_BSONOBJ_NE(obj1, obj2);
    ASSERT(comparator.evaluate(obj1 != obj2));
}

TEST(CollationBSONComparisonTest, IdenticalCodeAndStringValuesDoNotHashEqually) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const BSONObjComparator comparator(
        BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, &collator);
    BSONObj obj1 = BSON("a" << "foo");
    BSONObj obj2 = BSON("a" << BSONCode("foo"));
    ASSERT_NE(comparator.hash(obj1), comparator.hash(obj2));
    ASSERT_NE(SimpleBSONObjComparator::kInstance.hash(obj1),
              SimpleBSONObjComparator::kInstance.hash(obj2));
}

}  // namespace
}  // namespace mongo
