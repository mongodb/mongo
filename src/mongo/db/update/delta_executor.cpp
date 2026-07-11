// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/delta_executor.h"

#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo {

DeltaExecutor::ApplyResult DeltaExecutor::applyUpdate(
    UpdateExecutor::ApplyParams applyParams) const {
    const auto originalDoc = applyParams.element.getDocument().getObject();

    auto postImage =
        doc_diff::applyDiff(originalDoc, _diff, _mustCheckExistenceForInsertOperations);
    auto postImageHasId = postImage.hasField("_id");

    auto result = ObjectReplaceExecutor::applyReplacementUpdate(
        std::move(applyParams), postImage, postImageHasId);
    result.oplogEntry = _outputOplogEntry;

    // We could directly return the '_diff' object, but we would not be able to make any guarantees
    // about its lifetime to callers.
    if (auto diff = _outputOplogEntry[update_oplog_entry::kDiffObjectFieldName];
        diff.isABSONObj()) {
        result.diff = diff.embeddedObject();
    }
    return result;
}
}  // namespace mongo
