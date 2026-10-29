// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/projection_node.h"
#include "mongo/util/modules.h"

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

        // Pre-compute the hash once and reuse it for both set and map lookups.
        const auto hashedName = StringMapHasher{}.hashed_key(fieldName);

        if (this->_projectedFieldsSet.find(hashedName) != this->_projectedFieldsSet.end()) {
            projectionNode->_applyToProjectedField(bsonElement, bob);
            --nFieldsLeft;
        } else if (auto childIt = this->_children.find(hashedName);
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
