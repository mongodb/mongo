
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

#include <string>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class CollatorInterface;
class OperationContext;

class UpdateDriver {
public:
    UpdateDriver(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses the 'updateExpr' update expression into the '_root' member variable. Uasserts
     * if 'updateExpr' fails to parse.
     */
    void parse(const BSONObj& updateExpr,
               const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
               const bool multi = false);

    /**
     * Fills in document with any fields in the query which are valid.
     *
     * Valid fields include equality matches like "a":1, or "a.b":false
     *
     * Each valid field will be expanded (from dot notation) and conflicts will be
     * checked for all fields added to the underlying document.
     *
     * Returns Status::OK() if the document can be used. If there are any error or
     * conflicts along the way then those errors will be returned.
     *
     * If the current update is a document replacement, only the 'immutablePaths' are extracted from
     * 'query' and used to populate 'doc'.
     */
    Status populateDocumentWithQueryFields(OperationContext* opCtx,
                                           const BSONObj& query,
                                           const FieldRefSet& immutablePaths,
                                           mutablebson::Document& doc) const;

    Status populateDocumentWithQueryFields(const CanonicalQuery& query,
                                           const FieldRefSet& immutablePaths,
                                           mutablebson::Document& doc) const;

    /**
     * Executes the update over 'doc'. If any modifier is positional, use 'matchedField' (index of
     * the array item matched). If 'doc' allows the modifiers to be applied in place and no index
     * updating is involved, then the modifiers may be applied "in place" over 'doc'.
     *
     * If the driver's '_logOp' mode is turned on, and if 'logOpRec' is not null, fills in the
     * latter with the oplog entry corresponding to the update. If the modifiers can't be applied,
     * returns an error status or uasserts with a corresponding description.
     *
     * If 'validateForStorage' is true, ensures that modified elements do not violate depth or DBRef
     * constraints. Ensures that no paths in 'immutablePaths' are modified (though they may be
     * created, if they do not yet exist).
     *
     * If 'modifiedPaths' is not null, this method will populate it with the set of paths that were
     * either modified at runtime or present statically in the update modifiers. For arrays, the
     * set will include only the path to the array if the length has changed. All paths encode array
     * indexes explicitly.
     *
     * The caller must either provide a null pointer, or a non-null pointer to an empty field ref
     * set.
     */
    Status update(StringData matchedField,
                  mutablebson::Document* doc,
                  bool validateForStorage,
                  const FieldRefSet& immutablePaths,
                  BSONObj* logOpRec = nullptr,
                  bool* docWasModified = nullptr,
                  FieldRefSetWithStorage* modifiedPaths = nullptr);

    //
    // Accessors
    //

    bool isDocReplacement() const;
    static bool isDocReplacement(const BSONObj& updateExpr);

    bool modsAffectIndices() const;
    void refreshIndexKeys(const UpdateIndexData* indexedFields);

    bool logOp() const;
    void setLogOp(bool logOp);

    bool fromOplogApplication() const;
    void setFromOplogApplication(bool fromOplogApplication);

    void setInsert(bool insert) {
        _insert = insert;
    }

    mutablebson::Document& getDocument() {
        return _objDoc;
    }

    const mutablebson::Document& getDocument() const {
        return _objDoc;
    }

    bool needMatchDetails() const {
        return _positional;
    }

    /**
     * Set the collator which will be used by all of the UpdateDriver's underlying modifiers.
     *
     * 'collator' must outlive the UpdateDriver.
     */
    void setCollator(const CollatorInterface* collator);

private:
    /** Create the modifier and add it to the back of the modifiers vector */
    inline Status addAndParse(const modifiertable::ModifierType type, const BSONElement& elem);

    //
    // immutable properties after parsing
    //

    // Is this a full object replacement or do we have update modifiers in the '_root' UpdateNode
    // tree?
    bool _replacementMode = false;

    // The root of the UpdateNode tree.
    std::unique_ptr<UpdateNode> _root;

    // What are the list of fields in the collection over which the update is going to be
    // applied that participate in indices?
    //
    // NOTE: Owned by the collection's info cache!.
    const UpdateIndexData* _indexedFields = nullptr;

    //
    // mutable properties after parsing
    //

    // Should this driver generate an oplog record when it applies the update?
    bool _logOp = false;

    // True if this update comes from an oplog application.
    bool _fromOplogApplication = false;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Are any of the fields mentioned in the mods participating in any index? Is set anew
    // at each call to update.
    bool _affectIndices = false;

    // Do any of the mods require positional match details when calling 'prepare'?
    bool _positional = false;

    // Is this update going to be an upsert?
    bool _insert = false;

    // The document used to represent or store the object being updated.
    mutablebson::Document _objDoc;

    // The document used to build the oplog entry for the update.
    mutablebson::Document _logDoc;
};

}  // namespace mongo
