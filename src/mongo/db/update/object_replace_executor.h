// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo {

/**
 * An UpdateExecutor representing a replacement-style update.
 */
class ObjectReplaceExecutor : public UpdateExecutor {

public:
    /**
     * Applies a replacement style update to 'applyParams.element'.
     *
     * If 'replacementDocContainsIdField' is false then the _id field from the original document
     * will be preserved.
     *
     * If 'allowTopLevelDollarPrefixedFields' is true, top-level dollar-prefixed fields will be
     * permitted in document updates. This is only set to true in pipeline-style updates, when we
     * can be sure that there are no collisions between '$'-prefixed fieldnames and update modifiers
     * like $set.
     *
     * This function will ignore the log mode provided in 'applyParams'. The 'oplogEntry' field
     * of the returned ApplyResult is always empty.
     */
    static ApplyResult applyReplacementUpdate(ApplyParams applyParams,
                                              const BSONObj& replacementDoc,
                                              bool replacementDocContainsIdField,
                                              bool allowTopLevelDollarPrefixedFields = false);

    /**
     * Initializes the node with the document to replace with. If 'bypassEmptyTsReplacement' is
     * false, any zero-valued timestamps (except for the _id) will be replaced with the current
     * time.
     */
    explicit ObjectReplaceExecutor(BSONObj replacement, bool bypassEmptyTsReplacement = false);

    /**
     * Replaces the document that 'applyParams.element' belongs to with 'val'. If 'val' does not
     * contain an _id, the _id from the original document is preserved. 'applyParams.element' must
     * be the root of the document. Always returns a result stating that indexes are affected when
     * the replacement is not a noop.
     */
    ApplyResult applyUpdate(ApplyParams applyParams) const final;

    Value serialize() const final {
        return Value(_replacementDoc);
    }

    const BSONObj& getReplacement() const {
        return _replacementDoc;
    }

    void setReplacement(BSONObj doc) {
        _replacementDoc = std::move(doc);
    }

private:
    BSONObj _replacementDoc;

    // True if '_replacementDoc' contains an _id.
    bool _containsId;

    bool _bypassEmptyTsReplacement = false;
};

}  // namespace mongo
