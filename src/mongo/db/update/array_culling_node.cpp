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

#include "mongo/platform/basic.h"

#include "mongo/db/update/array_culling_node.h"

namespace mongo {

ModifierNode::ModifyResult ArrayCullingNode::updateExistingElement(
    mutablebson::Element* element, std::shared_ptr<FieldRef> elementPath) const {
    invariant(element->ok());
    uassert(ErrorCodes::BadValue,
            "Cannot apply $pull to a non-array value",
            element->getType() == mongo::Array);

    size_t numRemoved = 0;
    auto cursor = element->leftChild();
    while (cursor.ok()) {
        // Make sure to get the next array element now, because if we remove the 'cursor' element,
        // the rightSibling pointer will be invalidated.
        auto nextElement = cursor.rightSibling();
        if (_matcher->match(cursor)) {
            invariantOK(cursor.remove());
            numRemoved++;
        }
        cursor = nextElement;
    }

    return (numRemoved == 0) ? ModifyResult::kNoOp : ModifyResult::kNormalUpdate;
}

void ArrayCullingNode::validateUpdate(mutablebson::ConstElement updatedElement,
                                      mutablebson::ConstElement leftSibling,
                                      mutablebson::ConstElement rightSibling,
                                      std::uint32_t recursionLevel,
                                      ModifyResult modifyResult) const {
    invariant(modifyResult == ModifyResult::kNormalUpdate);

    // Removing elements from an array cannot increase BSON depth or modify a DBRef, so we can
    // override validateUpdate to do nothing.
}

}  // namespace mongo
