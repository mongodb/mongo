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

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"

namespace mongo {

/**
 * This class provides the query system with the ability to perform projections using the
 * aggregation system's projection semantics.
 */
class ProjectionExecAgg {
public:
    // Allows the caller to indicate whether the projection should default to including or excluding
    // the _id field in the event that the projection spec does not specify the desired behavior.
    // For instance, given a projection {a: 1}, specifying 'kExcludeId' is equivalent to projecting
    // {a: 1, _id: 0} while 'kIncludeId' is equivalent to the projection {a: 1, _id: 1}. If the user
    // explicitly specifies a projection on _id, then this will override the default policy; for
    // instance, {a: 1, _id: 0} will exclude _id for both 'kExcludeId' and 'kIncludeId'.
    enum class DefaultIdPolicy { kIncludeId, kExcludeId };

    // Allows the caller to specify how the projection should handle nested arrays; that is, an
    // array whose immediate parent is itself an array. For example, in the case of sample document
    // {a: [1, 2, [3, 4], {b: [5, 6]}]} the array [3, 4] is a nested array. The array [5, 6] is not,
    // because there is an intervening object between it and its closest array ancestor.
    enum class ArrayRecursionPolicy { kRecurseNestedArrays, kDoNotRecurseNestedArrays };

    enum class ProjectionType { kInclusionProjection, kExclusionProjection };

    static std::unique_ptr<ProjectionExecAgg> create(BSONObj projSpec,
                                                     DefaultIdPolicy defaultIdPolicy,
                                                     ArrayRecursionPolicy recursionPolicy);

    ~ProjectionExecAgg();

    BSONObj applyProjection(BSONObj inputDoc) const;

    stdx::unordered_set<std::string> applyProjectionToFields(
        const stdx::unordered_set<std::string>& fields) const;

    /**
     * Apply the projection to a single field name. Returns whether or not the projection would
     * allow that field to remain in a document.
     **/
    bool applyProjectionToOneField(StringData field) const;

    /**
     * Returns the exhaustive set of all paths that will be preserved by this projection, or an
     * empty set if the exhaustive set cannot be determined. An inclusion will always produce an
     * exhaustive set; an exclusion will always produce an empty set.
     */
    const std::set<FieldRef>& getExhaustivePaths() const;

    ProjectionType getType() const;

    BSONObj getProjectionSpec() const {
        return _projSpec;
    }

private:
    /**
     * ProjectionExecAgg::ProjectionExecutor wraps all agg-specific calls, and is forward-declared
     * here to avoid exposing any types from ParsedAggregationProjection to the query system.
     */
    class ProjectionExecutor;

    ProjectionExecAgg(BSONObj projSpec, std::unique_ptr<ProjectionExecutor> exec);

    std::unique_ptr<ProjectionExecutor> _exec;
    const BSONObj _projSpec;
};
}  // namespace mongo
