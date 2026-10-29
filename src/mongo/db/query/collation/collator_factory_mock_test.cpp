// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_factory_mock.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(CollatorFactoryMockTest, CollatorFactoryMockReturnsNullCollatorIfLocaleSimple) {
    CollatorFactoryMock factory;
    auto collator = factory.makeFromBSON(BSON("locale" << "simple"));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue());
}

TEST(CollatorFactoryMockTest, CollatorFactoryMockConstructsReverseStringCollator) {
    CollatorFactoryMock factory;
    auto collator = factory.makeFromBSON(BSONObj());
    ASSERT_OK(collator.getStatus());
    ASSERT_GT(collator.getValue()->compare("abc", "cba"), 0);
}

}  // namespace
