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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace bson_helper_details {
inline BSONFieldValue<BSONArray> makeOperator(std::string op, const auto&... args) {
    BSONArrayBuilder bab{kBSONObjBuilderInitialCapacity};
    (bab.append(args), ...);
    return {std::move(op), bab.arr()};
}
}  // namespace bson_helper_details

/** $or helper: BSON(OR(BSON("x" << GT << 7), BSON("y" << LT << 6)));
 * becomes   : {$or: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
 */
MONGO_MOD_PUB BSONFieldValue<BSONArray> OR(const auto&... args) {
    return bson_helper_details::makeOperator("$or", args...);
}

/**
 * $and helper: BSON(AND(BSON("x" << GT << 7), BSON("y" << LT << 6)));
 * becomes   : {$and: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
 */
MONGO_MOD_PUB inline BSONFieldValue<BSONArray> AND(const auto&... args) {
    return bson_helper_details::makeOperator("$and", args...);
}

/**
 * $nor helper: BSON(NOR(BSON("x" << GT << 7), BSON("y" << LT << 6)));
 * becomes   : {$nor: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
 */
MONGO_MOD_PUB inline BSONFieldValue<BSONArray> NOR(const auto&... args) {
    return bson_helper_details::makeOperator("$nor", args...);
}

}  // namespace mongo
