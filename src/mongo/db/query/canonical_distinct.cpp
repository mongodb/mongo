// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/canonical_distinct.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <string_view>


namespace mongo {

const char CanonicalDistinct::kKeyField[] = "key";
const char CanonicalDistinct::kQueryField[] = "query";
const char CanonicalDistinct::kCollationField[] = "collation";
const char CanonicalDistinct::kUnwoundArrayFieldForViewUnwind[] = "_internalUnwoundArray";
const char CanonicalDistinct::kHintField[] = "hint";

namespace {

/**
 * Helper for when converting a distinct() to an aggregation pipeline. This function may add a
 * $match stage enforcing that intermediate subpaths are objects so that no implicit array
 * traversal happens later on. The $match stage is only added when the path is dotted (e.g. "a.b"
 * but for "xyz").
 *
 * See comments in CanonicalDistinct::asAggregationCommand() for more detailed explanation.
 */
void addMatchRemovingNestedArrays(BSONArrayBuilder* pipelineBuilder, const FieldPath& unwindPath) {
    tassert(11320902, "unwindPath length cannot be 0", unwindPath.getPathLength() != 0);
    if (unwindPath.getPathLength() == 1) {
        return;
    }

    BSONObjBuilder matchBuilder(pipelineBuilder->subobjStart());
    BSONObjBuilder predicateBuilder(matchBuilder.subobjStart("$match"));


    for (size_t i = 0; i < unwindPath.getPathLength() - 1; ++i) {
        std::string_view pathPrefix = unwindPath.getSubpath(i);
        // Add a clause to the $match predicate requiring that intermediate paths are objects so
        // that no implicit array traversal happens.
        predicateBuilder.append(pathPrefix, BSON("$_internalSchemaType" << "object"));
    }

    predicateBuilder.doneFast();
    matchBuilder.doneFast();
}

}  // namespace
}  // namespace mongo
