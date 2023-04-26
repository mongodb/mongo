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

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/pipeline_executor.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/db/update/update_tree_executor.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class CollatorInterface;
class OperationContext;

class UpdateDriver {
public:
    enum class UpdateType { kOperator, kReplacement, kPipeline, kDelta, kTransform };

    UpdateDriver(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses the 'updateExpr' update expression into the '_updateExecutor' member variable.
     * Uasserts if 'updateExpr' fails to parse.
     */
    void parse(const write_ops::UpdateModification& updateExpr,
               const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
               boost::optional<BSONObj> constants = boost::none,
               bool multi = false);

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
     * The value of 'isInsert' controls whether $setOnInsert modifiers get applied.
     *
     * If 'modifiedPaths' is not null, this method will populate it with the set of paths that were
     * either modified at runtime or present statically in the update modifiers. For arrays, the
     * set will include only the path to the array if the length has changed. All paths encode array
     * indexes explicitly.
     *
     * The caller must either provide a null pointer, or a non-null pointer to an empty field ref
     * set.
     */
    Status update(OperationContext* opCtx,
                  StringData matchedField,
                  mutablebson::Document* doc,
                  bool validateForStorage,
                  const FieldRefSet& immutablePaths,
                  bool isInsert,
                  BSONObj* logOpRec = nullptr,
                  bool* docWasModified = nullptr,
                  FieldRefSetWithStorage* modifiedPaths = nullptr);

    /**
     * Passes the visitor through to the root of the update tree. The visitor is responsible for
     * implementing methods that operate on the nodes of the tree.
     */
    void visitRoot(UpdateNodeVisitor* visitor) {
        if (_updateType == UpdateType::kOperator) {
            UpdateTreeExecutor* exec = static_cast<UpdateTreeExecutor*>(_updateExecutor.get());
            invariant(exec);
            return exec->getUpdateTree()->acceptVisitor(visitor);
        }

        MONGO_UNREACHABLE;
    }

    UpdateExecutor* getUpdateExecutor() {
        return _updateExecutor.get();
    }

    //
    // Accessors
    //

    UpdateType type() const {
        return _updateType;
    }

    bool logOp() const {
        return _logOp;
    }
    void setLogOp(bool logOp) {
        _logOp = logOp;
    }

    bool fromOplogApplication() const {
        return _fromOplogApplication;
    }
    void setFromOplogApplication(bool fromOplogApplication) {
        _fromOplogApplication = fromOplogApplication;
    }

    void setSkipDotsDollarsCheck(bool skipDotsDollarsCheck) {
        _skipDotsDollarsCheck = skipDotsDollarsCheck;
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

    bool containsDotsAndDollarsField() const {
        return _containsDotsAndDollarsField;
    }

    void setContainsDotsAndDollarsField(const bool containsDotsAndDollarsField) {
        _containsDotsAndDollarsField = containsDotsAndDollarsField;
    }

    /**
     * Serialize the update expression to Value. Output of this method is expected to, when parsed,
     * produce a logically equivalent update expression.
     */
    Value serialize() const {
        return _updateExecutor->serialize();
    }

    /**
     * Set the collator which will be used by all of the UpdateDriver's underlying modifiers.
     *
     * 'collator' must outlive the UpdateDriver.
     */
    void setCollator(const CollatorInterface* collator);

private:
    /** Create the modifier and add it to the back of the modifiers vector */
    inline Status addAndParse(modifiertable::ModifierType type, const BSONElement& elem);

    //
    // immutable properties after parsing
    //

    UpdateType _updateType = UpdateType::kOperator;

    std::unique_ptr<UpdateExecutor> _updateExecutor;

    //
    // mutable properties after parsing
    //

    // Should this driver generate an oplog record when it applies the update?
    bool _logOp = false;

    // True if this update comes from an oplog application.
    bool _fromOplogApplication = false;

    // True if this update is guaranteed not to contain dots or dollars fields and should skip the
    // check.
    bool _skipDotsDollarsCheck = false;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Do any of the mods require positional match details when calling 'prepare'?
    bool _positional = false;

    // True if the updated document contains any '.'/'$' field name.
    bool _containsDotsAndDollarsField = false;

    // The document used to represent or store the object being updated.
    mutablebson::Document _objDoc;

    // The document used to build the oplog entry for the update.
    mutablebson::Document _logDoc;
};

}  // namespace mongo
