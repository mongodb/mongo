// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
[[MONGO_MOD_PUBLIC]] BSONFieldValue<BSONArray> OR(const auto&... args) {
    return bson_helper_details::makeOperator("$or", args...);
}

/**
 * $and helper: BSON(AND(BSON("x" << GT << 7), BSON("y" << LT << 6)));
 * becomes   : {$and: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
 */
[[MONGO_MOD_PUBLIC]] inline BSONFieldValue<BSONArray> AND(const auto&... args) {
    return bson_helper_details::makeOperator("$and", args...);
}

/**
 * $nor helper: BSON(NOR(BSON("x" << GT << 7), BSON("y" << LT << 6)));
 * becomes   : {$nor: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
 */
[[MONGO_MOD_PUBLIC]] inline BSONFieldValue<BSONArray> NOR(const auto&... args) {
    return bson_helper_details::makeOperator("$nor", args...);
}

}  // namespace mongo
