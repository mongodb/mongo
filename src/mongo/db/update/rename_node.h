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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/update/update_leaf_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/**
 * Represents the application of a $rename to the value at the end of a path, where the path
 * represents the rename destination.
 */
class RenameNode : public UpdateLeafNode {
public:
    /**
     * This init provides input validation on the source field (stored as the field name in
     * "modExpr") and the destination field (stored as the value in "modExpr").
     */
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<RenameNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    ApplyResult apply(ApplyParams applyParams) const final;

    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates) const final {
        // The RenameNode sits in the update tree at the destination path, because that's the path
        // that may need to be synthesized if it's not already in the document. However, the
        // destination path is the _value_ and goes on the right side of the rename element (i.e.:
        // {$rename: {sourcePath: "destPath"}}), unlike all other modifiers, where the path to
        // synthesize is the field (on the left).
        (*operatorOrientedUpdates)["$rename"].emplace_back(_val.fieldName(),
                                                           BSON("" << currentPath->dottedField()));
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

    BSONElement getValue() const {
        return _val;
    }

private:
    BSONElement _val;
};

}  // namespace mongo
