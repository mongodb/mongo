/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

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

