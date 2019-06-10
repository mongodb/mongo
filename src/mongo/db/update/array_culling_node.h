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

#include <memory>

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/update/modifier_node.h"

namespace mongo {

/**
 * An ArrayCullingNode represents an update modifier that removes elements from an array that match
 * a predicate (condensing the array in the process), such as $pull and $pullAll. Modifiers with
 * this behavior all use the same apply() logic (implemented in this class), which uses a
 * subclass-supplied ElementMatcher to determine which array elements should be removed. The init
 * method for each subclass must populate _matcher with an ElementMatcher implementation.
 */
class ArrayCullingNode : public ModifierNode {
public:
    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       std::shared_ptr<FieldRef> elementPath) const final;

    void validateUpdate(mutablebson::ConstElement updatedElement,
                        mutablebson::ConstElement leftSibling,
                        mutablebson::ConstElement rightSibling,
                        std::uint32_t recursionLevel,
                        ModifyResult modifyResult) const final;

    void setCollator(const CollatorInterface* collator) final {
        _matcher->setCollator(collator);
    }

protected:
    /**
     * ArrayCullingNode::apply() uses an ElementMatcher to determine which array elements meet the
     * $pull condition. The different subclasses of ElementMatcher implement the different kinds of
     * checks that can be used for a $pull operation.
     */
    class ElementMatcher {
    public:
        virtual ~ElementMatcher() = default;
        virtual std::unique_ptr<ElementMatcher> clone() const = 0;
        virtual bool match(const mutablebson::ConstElement& element) = 0;
        virtual void setCollator(const CollatorInterface* collator) = 0;
        /**
         * Retrieve the value the matcher applies as a single-element BSONObj with an empty string
         * as the keyname. For example, for the input syntax: { $pull: { votes: { $gte: 6 } } },
         * this function would return: { "": { $gte: 6 } } in BSON.
         */
        virtual BSONObj value() const = 0;
    };

    clonable_ptr<ElementMatcher> _matcher;

private:
    BSONObj operatorValue() const final {
        return _matcher->value();
    }
};

}  // namespace mongo
