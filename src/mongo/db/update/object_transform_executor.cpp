// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/object_transform_executor.h"

#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"

#include <utility>


namespace mongo {

ObjectTransformExecutor::ObjectTransformExecutor(TransformFunc transformFunc)
    : _transformFunc(std::move(transformFunc)) {}

UpdateExecutor::ApplyResult ObjectTransformExecutor::applyTransformUpdate(
    ApplyParams applyParams, const TransformFunc& transformFunc) {

    auto originalDoc = applyParams.element.getDocument().getObject();
    auto transformedDoc = transformFunc(originalDoc);
    if (!transformedDoc) {
        return ApplyResult::noopResult();
    }

    return ObjectReplaceExecutor::applyReplacementUpdate(
        std::move(applyParams),
        *transformedDoc,
        true /* replacementDocContainsIdField */,
        true /* allowTopLevelDollarPrefixedFields */);
}

UpdateExecutor::ApplyResult ObjectTransformExecutor::applyUpdate(ApplyParams applyParams) const {
    auto ret = applyTransformUpdate(applyParams, _transformFunc);

    if (!ret.noop && applyParams.logMode != ApplyParams::LogMode::kDoNotGenerateOplogEntry) {
        // Using a full replacement oplog entry as the current use case is doing major changes.
        // Consider supporting v:2 update in the future.
        ret.oplogEntry = update_oplog_entry::makeReplacementOplogEntry(
            applyParams.element.getDocument().getObject());
    }
    return ret;
}

Value ObjectTransformExecutor::serialize() const {
    MONGO_UNREACHABLE_TASSERT(5857810);
}
}  // namespace mongo
