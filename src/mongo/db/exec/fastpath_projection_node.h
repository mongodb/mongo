/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/projection_node.h"

#include <boost/optional.hpp>

namespace mongo::projection_executor {

/**
 * Generic fast-path projection implementation which applies a BSON-to-BSON transformation rather
 * than constructing an output document using the Document/Value API.
 */
template <typename ProjectionNode, typename BaseProjectionNode>
class FastPathProjectionNode : public BaseProjectionNode {
public:
    using BaseProjectionNode::BaseProjectionNode;

protected:
    /**
     * If document is eligible for fast path projection, apply it and return the resulting document.
     * Otherwise return boost::none. Caller will need to fall back to default implementation.
     */
    boost::optional<Document> tryApplyFastPathProjection(const Document& inputDoc) const;

private:
    void _applyProjections(BSONObj bson, BSONObjBuilder* bob) const;
    void _applyProjectionsToArray(BSONObj array, BSONArrayBuilder* bab) const;
};

template <typename ProjectionNode, typename BaseProjectionNode>
boost::optional<Document>
FastPathProjectionNode<ProjectionNode, BaseProjectionNode>::tryApplyFastPathProjection(
    const Document& inputDoc) const {
    tassert(7241741,
            "fast-path projections cannot contain computed fields",
            !this->_subtreeContainsComputedFields);

    // If we can get the backing BSON object off the input document without allocating an owned
    // copy, then we can apply a fast-path BSON-to-BSON exclusion projection.
    if (auto bson = inputDoc.toBsonIfTriviallyConvertible()) {
        BSONObjBuilder bob;
        _applyProjections(*bson, &bob);

        Document outputDoc{bob.obj()};
        // Make sure that we always pass through any metadata present in the input doc.
        if (inputDoc.metadata()) {
            MutableDocument md{std::move(outputDoc)};
            md.copyMetaDataFrom(inputDoc);
            return md.freeze();
        }
        return outputDoc;
    }

    return boost::none;
}

template <typename ProjectionNode, typename BaseProjectionNode>
void FastPathProjectionNode<ProjectionNode, BaseProjectionNode>::_applyProjections(
    BSONObj bson, BSONObjBuilder* bob) const {
    const auto* projectionNode = static_cast<const ProjectionNode*>(this);
    auto nFieldsLeft = this->_projectedFieldsSet.size() + this->_children.size();

    BSONObjIterator it{bson};
    while (it.more() && nFieldsLeft > 0) {
        const auto bsonElement{it.next()};
        const auto fieldName{bsonElement.fieldNameStringData()};

        if (this->_projectedFieldsSet.find(fieldName) != this->_projectedFieldsSet.end()) {
            projectionNode->_applyToProjectedField(bsonElement, bob);
            --nFieldsLeft;
        } else if (auto childIt = this->_children.find(fieldName);
                   childIt != this->_children.end()) {
            auto child = static_cast<const ProjectionNode*>(childIt->second.get());

            if (bsonElement.type() == BSONType::object) {
                BSONObjBuilder subBob{bob->subobjStart(fieldName)};
                child->_applyProjections(bsonElement.embeddedObject(), &subBob);
            } else if (bsonElement.type() == BSONType::array) {
                BSONArrayBuilder subBab{bob->subarrayStart(fieldName)};
                child->_applyProjectionsToArray(bsonElement.embeddedObject(), &subBab);
            } else {
                projectionNode->_applyToNonProjectedField(bsonElement, bob);
            }
            --nFieldsLeft;
        } else {
            projectionNode->_applyToNonProjectedField(bsonElement, bob);
        }
    }

    projectionNode->_applyToRemainingFields(it, bob);
}

template <typename ProjectionNode, typename BaseProjectionNode>
void FastPathProjectionNode<ProjectionNode, BaseProjectionNode>::_applyProjectionsToArray(
    BSONObj array, BSONArrayBuilder* bab) const {
    const auto* projectionNode = static_cast<const ProjectionNode*>(this);
    for (auto&& bsonElement : array) {
        if (bsonElement.type() == BSONType::object) {
            BSONObjBuilder subBob{bab->subobjStart()};
            _applyProjections(bsonElement.embeddedObject(), &subBob);
        } else if (bsonElement.type() == BSONType::array) {
            if (this->_policies.arrayRecursionPolicy ==
                ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays) {
                projectionNode->_applyToNonProjectedField(bsonElement, bab);
            } else {
                BSONArrayBuilder subBab{bab->subarrayStart()};
                _applyProjectionsToArray(bsonElement.embeddedObject(), &subBab);
            }
        } else {
            projectionNode->_applyToNonProjectedField(bsonElement, bab);
        }
    }
}

}  // namespace mongo::projection_executor
