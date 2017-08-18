/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/update/update_leaf_node.h"
#include "mongo/stdx/memory.h"

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

    ApplyResult apply(ApplyParams applyParams) const final;

protected:
    enum class ModifyResult {
        // No log entry is necessary for no-op updates.
        kNoOp,

        // The update can be logged as normal (usually with a $set on the entire element).
        kNormalUpdate,

        // The element is an array, and the update only appends new array items to the end. The
        // update can be logged as a $set on each appended element, rather than including the entire
        // array in the log entry.
        kArrayAppendUpdate,

        // The element did not exist, so it was created then populated with setValueForNewElement().
        // The updateExistingElement() method should never return this value.
        kCreated
    };

    /**
     * ModifierNode::apply() calls this method when applying an update to an existing path. The
     * child's implementation of this method is responsible for updating 'element' and indicating
     * what kind of update was performed in its return value.
     */
    virtual ModifyResult updateExistingElement(mutablebson::Element* element,
                                               std::shared_ptr<FieldRef> elementPath) const = 0;

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
     * storage_validation::storageValid() for more detail about document validation requirements.
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
     */
    virtual void validateUpdate(mutablebson::ConstElement updatedElement,
                                mutablebson::ConstElement leftSibling,
                                mutablebson::ConstElement rightSibling,
                                std::uint32_t recursionLevel,
                                ModifyResult modifyResult) const;

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
     */
    virtual void logUpdate(LogBuilder* logBuilder,
                           StringData pathTaken,
                           mutablebson::Element element,
                           ModifyResult modifyResult) const;

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

private:
    ApplyResult applyToNonexistentElement(ApplyParams applyParams) const;
    ApplyResult applyToExistingElement(ApplyParams applyParams) const;
};

}  // namespace mongo
