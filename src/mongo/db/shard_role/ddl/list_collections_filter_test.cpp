// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/list_collections_filter.h"

#include "mongo/bson/json.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace {

using namespace mongo;

TEST(ListCollectionsFilterTest, AddCollectionFilter) {
    BSONObj filter = fromjson("{'options.capped': false}");
    BSONObj expected = fromjson(
        "{$and: ["
        "{'options.capped': false}, "
        "{$or: [{type: 'collection'}, {type: {$exists: false}}]}]}");

    ASSERT_BSONOBJ_EQ(expected, ListCollectionsFilter::addTypeCollectionFilter(filter));
}

TEST(ListCollectionsFilterTest, AddCollectionFilterToEmptyInputFilter) {
    BSONObj filter;
    BSONObj expected = fromjson("{$or: [{type: 'collection'}, {type: {$exists: false}}]}");

    ASSERT_BSONOBJ_EQ(expected, ListCollectionsFilter::addTypeCollectionFilter(filter));
}

TEST(ListCollectionsFilterTest, AddViewFilter) {
    BSONObj filter = fromjson("{'options.capped': false}");
    BSONObj expected = fromjson("{$and: [{'options.capped': false}, {type: 'view'}]}");

    ASSERT_BSONOBJ_EQ(expected, ListCollectionsFilter::addTypeViewFilter(filter));
}

TEST(ListCollectionsFilterTest, AddViewFilterToEmptyInputFilter) {
    BSONObj filter;
    BSONObj expected = fromjson("{type: 'view'}");

    ASSERT_BSONOBJ_EQ(expected, ListCollectionsFilter::addTypeViewFilter(filter));
}

TEST(ListCollectionsFilterTest, MakeExcludeViewsFilter) {
    BSONObj expected = fromjson("{type: {$ne: 'view'}}");

    ASSERT_BSONOBJ_EQ(expected, ListCollectionsFilter::makeExcludeViewsFilter());
}

}  // namespace
