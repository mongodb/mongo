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

#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/**
 * Represents the application of an $addToSet to the value at the end of a path.
 */
class AddToSetNode : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<AddToSetNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final;

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

protected:
    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       std::shared_ptr<FieldRef> elementPath) const final;
    void setValueForNewElement(mutablebson::Element* element) const final;

    bool allowCreation() const final {
        return true;
    }

private:
    StringData operatorName() const final {
        return "$addToSet";
    }

    BSONObj operatorValue() const final {
        if (_elements.size() == 1) {
            return BSON("" << _elements[0]);
        } else {
            BSONObjBuilder bob;
            {
                BSONObjBuilder subBuilder(bob.subobjStart(""));
                {
                    BSONObjBuilder eachBuilder(subBuilder.subarrayStart("$each"));
                    for (const auto element : _elements)
                        eachBuilder << element;
                }
            }
            return bob.obj();
        }
    }

    // The array of elements to be added.
    std::vector<BSONElement> _elements;

    // The collator used to compare array elements for deduplication.
    const CollatorInterface* _collator = nullptr;
};

}  // namespace mongo
