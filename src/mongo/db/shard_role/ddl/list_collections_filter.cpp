// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/list_collections_filter.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

// TODO SERVER-25493: Remove $exists clause once MongoDB versions <= 3.2 are no longer supported.
BSONObj ListCollectionsFilter::makeTypeCollectionFilter() {
    return BSON("$or" << BSON_ARRAY(BSON("type" << "collection")
                                    << BSON("type" << BSON("$exists" << false))));
}

BSONObj ListCollectionsFilter::makeTypeViewFilter() {
    return BSON("type" << "view");
}

BSONObj ListCollectionsFilter::makeExcludeViewsFilter() {
    return BSON("type" << BSON("$ne" << "view"));
}

BSONObj ListCollectionsFilter::addTypeCollectionFilter(const BSONObj& filter) {
    if (filter.isEmpty())
        return makeTypeCollectionFilter();

    return BSON("$and" << BSON_ARRAY(filter << makeTypeCollectionFilter()));
}

BSONObj ListCollectionsFilter::addTypeViewFilter(const BSONObj& filter) {
    if (filter.isEmpty())
        return makeTypeViewFilter();

    return BSON("$and" << BSON_ARRAY(filter << makeTypeViewFilter()));
}

}  // namespace mongo
