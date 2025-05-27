/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/update_executor.h"

#include <functional>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * An UpdateExecutor representing a transform-style update.
 *
 * A transform-style update is similar to a replacement-style update with the difference that the
 * replacement document is calculated from the original document via a user-provided function. In
 * case of a WriteConflict or similar where the update needs to be retried the output document will
 * be re-calculated.
 *
 * The replacement function MUST preserve the _id element from the original document.
 *
 * If the replacement function returns boost::none the update operation will be considered a no-op.
 */
class ObjectTransformExecutor : public UpdateExecutor {
public:
    using TransformFunc = std::function<boost::optional<BSONObj>(const BSONObj&)>;

    /**
     * Applies a transform style update to 'applyParams.element'.
     *
     * The transform function MUST preserve the _id element in the original document.
     *
     * This function will ignore the log mode provided in 'applyParams'. The 'oplogEntry' field
     * of the returned ApplyResult is always empty.
     */
    static ApplyResult applyTransformUpdate(ApplyParams applyParams,
                                            const TransformFunc& transformFunc);

    /**
     * Initializes the node with the transform function to apply to the original document.
     */
    explicit ObjectTransformExecutor(TransformFunc transform);

    /**
     * Replaces the document that 'applyParams.element' belongs to with result of transform
     * function. 'applyParams.element' must be the root of the document. Always returns a result
     * stating that indexes are affected when the replacement is not a noop.
     */
    ApplyResult applyUpdate(ApplyParams applyParams) const final;

    Value serialize() const final;

private:
    TransformFunc _transformFunc;
};

}  // namespace mongo
