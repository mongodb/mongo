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

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class PullNode final : public UpdateLeafNode {
public:
    Status init(BSONElement modExpr, const CollatorInterface* collator) final;

    void apply(mutablebson::Element element,
               FieldRef* pathToCreate,
               FieldRef* pathTaken,
               StringData matchedField,
               bool fromReplication,
               bool validateForStorage,
               const FieldRefSet& immutablePaths,
               const UpdateIndexData* indexData,
               LogBuilder* logBuilder,
               bool* indexesAffected,
               bool* noop) const final;

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<PullNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        _matcher->setCollator(collator);
    }

private:
    /**
     * PullNode::apply() uses an ElementMatcher to determine which array elements meet the $pull
     * condition. The different subclasses of ElementMatcher implement the different kinds of checks
     * that can be used for a $pull operation.
     */
    class ElementMatcher {
    public:
        virtual ~ElementMatcher() = default;
        virtual std::unique_ptr<ElementMatcher> clone() const = 0;
        virtual bool match(mutablebson::ConstElement element) = 0;
        virtual void setCollator(const CollatorInterface* collator) = 0;
    };

    class ObjectMatcher;
    class WrappedObjectMatcher;
    class EqualityMatcher;

    clonable_ptr<ElementMatcher> _matcher;
};

}  // namespace mongo
