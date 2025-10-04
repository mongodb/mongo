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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/update_executor.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
