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

#include "mongo/db/update/addtoset_node.h"

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

namespace {

/**
 * Deduplicates 'elements' using 'collator' for comparisons.
 */
void deduplicate(std::vector<BSONElement>& elements, const CollatorInterface* collator) {

    // Copy 'elements' into a new vector.
    std::vector<BSONElement> elementsCopy = elements;
    elements.clear();

    // Keep track of which elements were already added.
    BSONElementSet added(collator);

    // Copy all non-duplicate elements back into 'elements'. Ensure that the original order of
    // elements is preserved.
    for (auto&& elem : elementsCopy) {
        if (added.find(elem) == added.end()) {
            elements.push_back(elem);
        }
        added.insert(elem);
    }
}

}  // namespace

Status AddToSetNode::init(BSONElement modExpr,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    bool isEach = false;

    // If the value of 'modExpr' is an object whose first field is '$each', treat it as an $each.
    if (modExpr.type() == BSONType::Object) {
        auto firstElement = modExpr.Obj().firstElement();
        if (firstElement && firstElement.fieldNameStringData() == "$each") {
            isEach = true;
            if (firstElement.type() != BSONType::Array) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream()
                        << "The argument to $each in $addToSet must be an array but it was of type "
                        << typeName(firstElement.type()));
            }
            if (modExpr.Obj().nFields() > 1) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Found unexpected fields after $each in $addToSet: "
                                            << modExpr.Obj());
            }
            _elements = firstElement.Array();
        }
    }

    // If the value of 'modExpr' was not an $each, we append the entire element.
    if (!isEach) {
        _elements.push_back(modExpr);
    }

    setCollator(expCtx->getCollator());
    return Status::OK();
}

void AddToSetNode::setCollator(const CollatorInterface* collator) {
    invariant(!_collator);
    _collator = collator;
    deduplicate(_elements, _collator);
}

ModifierNode::ModifyResult AddToSetNode::updateExistingElement(
    mutablebson::Element* element, std::shared_ptr<FieldRef> elementPath) const {
    uassert(ErrorCodes::BadValue,
            str::stream() << "Cannot apply $addToSet to non-array field. Field named '"
                          << element->getFieldName()
                          << "' has non-array type "
                          << typeName(element->getType()),
            element->getType() == BSONType::Array);

    // Find the set of elements that do not already exist in the array 'element'.
    std::vector<BSONElement> elementsToAdd;
    for (auto&& elem : _elements) {
        auto shouldAdd = true;
        for (auto existingElem = element->leftChild(); existingElem.ok();
             existingElem = existingElem.rightSibling()) {
            if (existingElem.compareWithBSONElement(elem, _collator, false) == 0) {
                shouldAdd = false;
                break;
            }
        }
        if (shouldAdd) {
            elementsToAdd.push_back(elem);
        }
    }

    if (elementsToAdd.empty()) {
        return ModifyResult::kNoOp;
    }

    for (auto&& elem : elementsToAdd) {
        auto toAdd = element->getDocument().makeElement(elem);
        invariantOK(element->pushBack(toAdd));
    }

    return ModifyResult::kNormalUpdate;
}

void AddToSetNode::setValueForNewElement(mutablebson::Element* element) const {
    BSONObj emptyArray;
    invariantOK(element->setValueArray(emptyArray));
    for (auto&& elem : _elements) {
        auto toAdd = element->getDocument().makeElement(elem);
        invariantOK(element->pushBack(toAdd));
    }
}

}  // namespace mongo
