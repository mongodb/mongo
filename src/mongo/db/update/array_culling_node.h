// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

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
                                       const FieldRef& elementPath) const final;

    void validateUpdate(mutablebson::ConstElement updatedElement,
                        mutablebson::ConstElement leftSibling,
                        mutablebson::ConstElement rightSibling,
                        std::uint32_t recursionLevel,
                        ModifyResult modifyResult,
                        bool validateForStorage,
                        bool* containsDotsAndDollarsField,
                        bool fromOplogApplication) const final;

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
         * this function would return: { "": { $gte: 6 } } in BSON, also depending on what
         * serialization options are provided, i.e. { $gte: 1 } for representative or { $gte:
         * "?number" } for debug query shape serialization.
         */
        virtual BSONObj value(const query_shape::SerializationOptions& opts) const = 0;
    };

    clonable_ptr<ElementMatcher> _matcher;

private:
    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        return _matcher->value(opts);
    }
};

}  // namespace mongo
