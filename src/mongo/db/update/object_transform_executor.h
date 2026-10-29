// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/util/modules.h"

#include <functional>

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
