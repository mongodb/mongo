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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The apply() function for an update modifier must
 *   1) update the element at the update path if it exists;
 *   2) raise a user error, indicate a no-op, or create and initialize a new element if the update
 *      path does not exist, depending on the specific modifier semantics;
 *   3) check for any changes that modify an immutable path;
 *   4) determine whether indexes may be affected;
 *   5) validate that the updated document is valid for storage; and
 *   6) create an oplog entry for the update.
 * ModifierNode provides a generic implementation of the apply() function that does all this heavy
 * lifting and calls out to modifier-specific overrides that customize apply() behavior where
 * necessary.
 */
class ModifierNode : public UpdateLeafNode {
public:
    explicit ModifierNode(Context context = Context::kAll) : UpdateLeafNode(context) {}

    ApplyResult apply(ApplyParams applyParams,
                      UpdateNodeApplyParams updateNodeApplyParams) const final;

protected:
    struct ModifyResult {
        enum class Type {
            // No log entry is necessary for no-op updates.
            kNoOp,

            // The update can be logged as normal.
            kNormalUpdate,

            // The element is an array, and the update only appends new array items to the end.
            kArrayAppendUpdate,

            // The element did not exist, so it was created then populated with
            // setValueForNewElement().
            // The updateExistingElement() method should never return this value.
            kCreated
        };
        static const Type kNoOp = Type::kNoOp;
        static const Type kNormalUpdate = Type::kNormalUpdate;
        static const Type kArrayAppendUpdate = Type::kArrayAppendUpdate;
        static const Type kCreated = Type::kCreated;

        struct EmptyDescription {};
        struct ArrayAppendUpdateDescription {
            size_t inserted;
        };

        ModifyResult() {};
        ModifyResult(ModifyResult::Type type) : type(type) {}

        Type type;
        std::variant<EmptyDescription, ArrayAppendUpdateDescription> description =
            EmptyDescription{};
    };

    /**
     * ModifierNode::apply() calls this method when applying an update to an existing path. The
     * child's implementation of this method is responsible for updating 'element' and indicating
     * what kind of update was performed in its return value.
     */
    virtual ModifyResult updateExistingElement(mutablebson::Element* element,
                                               const FieldRef& elementPath) const = 0;

    /**
     * ModifierNode::apply() calls this method when applying an update to a path that does not yet
     * exist and should be created. The child's implementation of this method is responsible for
     * assigning a value to the new element, which will initially be null.
     *
     * This method only gets called when the child class implementation of allowCreation() returns
     * true. ModiferNode child classes should override setValueForNewElement() iff allowCreation()
     * returns true.
     */
    virtual void setValueForNewElement(mutablebson::Element* element) const {
        // Only implementations that return true for allowCreation() will override this method.
        MONGO_UNREACHABLE;
    };

    /**
     * ModifierNode::apply() calls this method after it finishes applying its update to validate
     * that no changes resulted in an invalid document. See the implementation of
     * storage_validation::scanDocument() for more detail about document validation requirements.
     * Most ModifierNode child classes can use the default implementation of this method.
     *
     * - 'updatedElement' is the element that was set by either updateExistingElement() or
     *   setValueForNewElement().
     * - 'leftSibling' and 'rightSibling' are the left and right siblings of 'updatedElement' or
     *   are invalid if 'updatedElement' is the first or last element in its object, respectively.
     *   If a ModifierNode deletes an element as part of its update (as $unset does),
     *   'updatedElement' will be invalid, but validateUpdate() can use the siblings to perform
     *   validation instead.
     * - 'recursionLevel' is the document nesting depth of the 'updatedElement' field.
     * - 'modifyResult' is either the value returned by updateExistingElement() or the value
     *    ModifyResult::kCreated.
     * - If 'validateForStorage' is true, we should verify that the updated element is valid for
     *   storage.
     * - 'containsDotsAndDollarsField' is true if 'updatedElement' contains any dots/dollars field.
     */
    virtual void validateUpdate(mutablebson::ConstElement updatedElement,
                                mutablebson::ConstElement leftSibling,
                                mutablebson::ConstElement rightSibling,
                                std::uint32_t recursionLevel,
                                ModifyResult modifyResult,
                                bool validateForStorage,
                                bool* containsDotsAndDollarsField) const;

    /**
     * ModifierNode::apply() calls this method after validation to create an oplog entry for the
     * applied update. Most ModifierNode child classes can use the default implementation of this
     * method, which creates a $set entry using 'pathTaken' for the path and 'element' for the
     * value.
     *
     * - 'logBuilder' provides the interface for appending log entries.
     * - 'pathTaken' is the path of the applied update.
     * - 'element' is the element that was set by either updateExistingElement() or
     *   setValueForNewElement().
     * - 'modifyResult' is either the value returned by updateExistingElement() or the value
     *    ModifyResult::kCreated.
     * - 'createdFieldIdx' indicates what the first component in 'pathTaken' is that was created as
     *   part of this update. If the update did not add any new fields, boost::none should be
     *   provided.
     */
    virtual void logUpdate(LogBuilderInterface* logBuilder,
                           const RuntimeUpdatePath& pathTaken,
                           mutablebson::Element element,
                           ModifyResult modifyResult,
                           boost::optional<int> createdFieldIdx) const;

    /**
     * ModifierNode::apply() calls this method to determine what to do when applying an update to a
     * path that does not exist. If the child class overrides this method to return true,
     * ModifierNode::apply() will create a new element at the path and call setValueForNewElement()
     * on it. Child classes must implement setValueForNewElement() iff this function returns true.
     */
    virtual bool allowCreation() const {
        return false;
    }

    /**
     * When allowCreation() returns false, ModiferNode::apply() calls this method when determining
     * if an update to a non-existent path is a no-op or an error. When allowNonViablePath() is
     * false, an update to a path that could be created (i.e. a "viable" path) is a no-op, and an
     * update to a path that could not be created results in a PathNotViable user error. When
     * allowNonViablePath() is true, there is no viabilty check, and any update to a nonexistent
     * path is a no-op.
     */
    virtual bool allowNonViablePath() const {
        return false;
    }

    /**
     * When it is possible for updateExistingElement to replace the element's contents with an
     * object value (e.g., {$set: {a: {c: 1}}} applied to the document {a: {b:1}}),
     * ModifierNode::apply() has to do extra work to determine if any immutable paths have been
     * modified. Any ModifierNode child class that can perform such an update should override this
     * method to return true, so that ModifierNode::apply() knows to do the extra checks.
     */
    virtual bool canSetObjectValue() const {
        return false;
    }

    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates) const override {
        (*operatorOrientedUpdates)[std::string{operatorName()}].emplace_back(
            currentPath->dottedField(), operatorValue());
    }

private:
    /**
     * Retrieve the name of the operator this node represents in input syntax. For example, for the
     * input syntax: {$set: {a: 3}}, this function would return "$set".
     */
    virtual StringData operatorName() const = 0;

    /**
     * Retrieve the value this operator applies as a single-element BSONObj with an empty string as
     * the keyname. For example, for the input syntax: {$set: {a: 3}}, this function would return:
     * {"": 3} in BSON.
     */
    virtual BSONObj operatorValue() const = 0;

    ApplyResult applyToNonexistentElement(ApplyParams applyParams,
                                          UpdateNodeApplyParams updateNodeApplyParams) const;
    ApplyResult applyToExistingElement(ApplyParams applyParams,
                                       UpdateNodeApplyParams updateNodeApplyParams) const;
};

}  // namespace mongo
