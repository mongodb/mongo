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

#include "mongo/db/pipeline/field_path.h"

namespace mongo {

/**
 * The physical representation of a join predicate between two relations. This struct is used by
 * QuerySolutionNodes that implement binary joins to indicate which fields are being joined.
 */
struct JoinPredicate {
    enum class ComparisonOp {
        Eq,
    };

    ComparisonOp op;
    // The left and right fields of the equality predicate. The order of left and right fields is
    // important as it corresponds to the children of a join node. The field may correspond directly
    // to a collection, in which case the namespace of the field is implicit in the structure of the
    // QSN, or to a stream of documetns which themselves represent the result of a join, in which
    // case the field will have a prefix representing the "as" field of a $lookup.
    FieldPath leftField;
    FieldPath rightField;
};

}  // namespace mongo
