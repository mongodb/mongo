/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/mutable/element.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class CollatorInterface;
class FieldRef;

/**
 * Provides an interface for applying an update to a document.
 */
class UpdateExecutor {
public:
    /**
     * The parameters required by UpdateExecutor::applyUpdate.
     */
    struct ApplyParams {
        /**
         * Enum indicating whether/what kind of oplog entry should be returned in the ApplyResult
         * by the update executor.
         */
        enum class LogMode {
            // Indicates that no oplog entry should be produced.
            kDoNotGenerateOplogEntry,

            // Indicates that the update executor should produce an oplog entry, and may use any
            // format.
            kGenerateOplogEntry
        };

        ApplyParams(mutablebson::Element element, const FieldRefSet& immutablePaths)
            : element(element), immutablePaths(immutablePaths) {}

        // The element to update.
        mutablebson::Element element;

        // 'applyUpdate' will uassert if it modifies an immutable path.
        const FieldRefSet& immutablePaths;

        // If there was a positional ($) element in the update expression, 'matchedField' is the
        // index of the array element that caused the query to match the document.
        StringData matchedField;

        // True if the update is being applied to a document to be inserted.
        bool insert = false;

        // This is provided because some modifiers may ignore certain errors when the update is from
        // replication.
        bool fromOplogApplication = false;

        // If true, it is guaranteed that the document doesn't contain dots or dollars fields and
        // should skip the check.
        bool skipDotsDollarsCheck = false;

        // If true, UpdateNode::apply ensures that modified elements do not violate depth or DBRef
        // constraints.
        bool validateForStorage = true;

        // Used to determine whether indexes are affected.
        const UpdateIndexData* indexData = nullptr;

        // Indicates whether/what type of oplog entry should be produced by the update executor.
        // If 'logMode' indicates an oplog entry should be produced but the update turns out to be
        // a noop, an oplog entry may not be produced.
        LogMode logMode = LogMode::kDoNotGenerateOplogEntry;

        // If provided, UpdateNode::apply will populate this with a path to each modified field.
        FieldRefSetWithStorage* modifiedPaths = nullptr;
    };

    /**
     * The outputs of apply().
     */
    struct ApplyResult {
        static ApplyResult noopResult() {
            ApplyResult applyResult;
            applyResult.indexesAffected = false;
            applyResult.noop = true;
            applyResult.containsDotsAndDollarsField = false;
            return applyResult;
        }

        bool indexesAffected = true;
        bool noop = false;
        bool containsDotsAndDollarsField = false;

        // The oplog entry to log. This is only populated if the operation is not considered a
        // noop and if the 'logMode' provided in ApplyParams indicates that an oplog entry should
        // be generated.
        BSONObj oplogEntry;
    };


    UpdateExecutor() = default;
    virtual ~UpdateExecutor() = default;

    virtual Value serialize() const = 0;

    virtual void setCollator(const CollatorInterface* collator){};

    /**
     * Applies the update to 'applyParams.element'. Returns an ApplyResult specifying whether the
     * operation was a no-op and whether indexes are affected.
     */
    virtual ApplyResult applyUpdate(ApplyParams applyParams) const = 0;
};

}  // namespace mongo
