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
 * A PathCreatingNode represents an update modifier that materializes the field it wants to update
 * when that field does not already exist. This category of modifiers includes $set, $inc, and $mul.
 * These nodes are very similar in operation, so most of their logic is in the apply() method of
 * this abstract base class, which calls into a virtual methods for modifier-specific functionality.
 */
class PathCreatingNode : public UpdateLeafNode {
public:
    void apply(mutablebson::Element element,
               FieldRef* pathToCreate,
               FieldRef* pathTaken,
               StringData matchedField,
               bool fromReplication,
               const UpdateIndexData* indexData,
               LogBuilder* logBuilder,
               bool* indexesAffected,
               bool* noop) const final;

protected:
    /**
     * PathCreatingNode::apply() calls the updateExistingElement() method when applying its update
     * to an existing path. The child's implementation of this method is responsible for either
     * updating the given Element or setting *noop to true to indicate that no update is necessary.
     */
    virtual void updateExistingElement(mutablebson::Element* element, bool* noop) const = 0;

    /**
     * PathCreatingNode::apply() calls the setValueForNewElement() method when it must materialize a
     * new field in order to apply its update. The child's implemenation of this method is
     * responsible for assigning a value to the new element (which will initially be null).
     */
    virtual void setValueForNewElement(mutablebson::Element* element) const = 0;
};

}  // namespace mongo
