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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"

#include <cstdint>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class PopNode final : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       const FieldRef& elementPath) const final;

    void validateUpdate(mutablebson::ConstElement updatedElement,
                        mutablebson::ConstElement leftSibling,
                        mutablebson::ConstElement rightSibling,
                        std::uint32_t recursionLevel,
                        ModifyResult modifyResult,
                        bool validateForStorage,
                        bool* containsDotsAndDollarsField) const final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<PopNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    /**
     * Returns true if this node is popping the first element off the front of the array. Returns
     * false if this node is popping the last element off the back of the array.
     */
    bool popFromFront() const {
        return _popFromFront;
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    StringData operatorName() const final {
        return "$pop";
    }

    BSONObj operatorValue() const final {
        return _popFromFront ? BSON("" << -1) : BSON("" << 1);
    }

    bool _popFromFront = true;
};

}  // namespace mongo
