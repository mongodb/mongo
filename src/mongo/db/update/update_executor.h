// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

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
        std::string_view matchedField;

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
            applyResult.noop = true;
            applyResult.containsDotsAndDollarsField = false;
            return applyResult;
        }

        bool noop = false;
        bool containsDotsAndDollarsField = false;

        // The oplog entry to log. This is only populated if the operation is not considered a
        // noop and if the 'logMode' provided in ApplyParams indicates that an oplog entry should
        // be generated.
        //
        // When populated, 'oplogEntry' is owned BSON.
        BSONObj oplogEntry;

        // The diff used to produce the oplog entry, if the oplog entry is a $v:2 delta entry.
        // Populated whenever oplogEntry is a delta entry.
        //
        // NOTE: 'diff' may be a view into BSON owned by 'oplogEntry' and should only be treated as
        // valid so long as 'oplogEntry' is valid.
        boost::optional<BSONObj> diff;
    };


    UpdateExecutor() = default;
    virtual ~UpdateExecutor() = default;

    virtual Value serialize() const = 0;

    virtual void setCollator(const CollatorInterface* collator) {};

    virtual bool getCheckExistenceForDiffInsertOperations() const {
        return false;
    }

    /**
     * Applies the update to 'applyParams.element'. Returns an ApplyResult specifying whether the
     * operation was a no-op and whether indexes are affected.
     */
    virtual ApplyResult applyUpdate(ApplyParams applyParams) const = 0;
};

}  // namespace mongo
