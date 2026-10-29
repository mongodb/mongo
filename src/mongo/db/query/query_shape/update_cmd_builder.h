// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo {
namespace query_shape {

/**
 * Helper struct for constructing UpdateCommandRequest BSON in tests and benchmarks.
 */
struct UpdateCmdBuilder {
    std::string database;
    std::string collection;
    boost::optional<bool> multi;
    boost::optional<bool> upsert;
    BSONObj q;
    BSONObj u;
    boost::optional<BSONObj> c;
    BSONObj let = BSONObj();
    BSONObj collation = BSONObj();
    boost::optional<BSONObj> arrayFilters;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("update", collection);
        BSONArrayBuilder updates(builder.subarrayStart("updates"));
        BSONObjBuilder updateObj;
        updateObj.append("q", q);

        if (u.couldBeArray()) {
            updateObj.appendArray("u", u);
        } else {
            updateObj.append("u", u);
        }
        if (c.has_value()) {
            updateObj.append("c", *c);
        }
        if (arrayFilters.has_value()) {
            updateObj.appendArray("arrayFilters", *arrayFilters);
        }
        if (multi.has_value()) {
            updateObj.append("multi", *multi);
        }
        if (upsert.has_value()) {
            updateObj.append("upsert", *upsert);
        }
        if (!collation.isEmpty()) {
            updateObj.append("collation", collation);
        }

        updates.append(updateObj.obj());
        updates.done();
        if (!let.isEmpty()) {
            builder.append("let", let);
        }
        builder.append("$db", database);
        return builder.obj();
    }
};

}  // namespace query_shape
}  // namespace mongo

