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

#include "mongo/db/local_catalog/ddl/list_collections_filter.h"

#include "mongo/base/string_data.h"
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
