/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/inclusion_projection_executor.h"

namespace mongo::projection_executor {
Document FastPathEligibleInclusionNode::applyToDocument(const Document& inputDoc) const {
    // A fast-path inclusion projection supports inclusion-only fields, so make sure we have no
    // computed fields in the specification.
    invariant(!_subtreeContainsComputedFields);

    // If we can get the backing BSON object off the input document without allocating an owned
    // copy, then we can apply a fast-path BSON-to-BSON inclusion projection.
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

    // A fast-path projection is not feasible, fall back to default implementation.
    return InclusionNode::applyToDocument(inputDoc);
}

void FastPathEligibleInclusionNode::_applyProjections(BSONObj bson, BSONObjBuilder* bob) const {
    auto nFieldsNeeded = _projectedFields.size() + _children.size();

    BSONObjIterator it{bson};
    while (it.more() && nFieldsNeeded > 0) {
        const auto bsonElement{it.next()};
        const auto fieldName{bsonElement.fieldNameStringData()};
        const absl::string_view fieldNameKey{fieldName.rawData(), fieldName.size()};

        if (_projectedFields.find(fieldNameKey) != _projectedFields.end()) {
            bob->append(bsonElement);
            --nFieldsNeeded;
        } else if (auto childIt = _children.find(fieldNameKey); childIt != _children.end()) {
            auto child = static_cast<FastPathEligibleInclusionNode*>(childIt->second.get());

            if (bsonElement.type() == BSONType::Object) {
                BSONObjBuilder subBob{bob->subobjStart(fieldName)};
                child->_applyProjections(bsonElement.embeddedObject(), &subBob);
            } else if (bsonElement.type() == BSONType::Array) {
                BSONArrayBuilder subBab{bob->subarrayStart(fieldName)};
                child->_applyProjectionsToArray(bsonElement.embeddedObject(), &subBab);
            } else {
                // The projection semantics dictate to exclude the field in this case if it
                // contains a scalar.
            }
            --nFieldsNeeded;
        }
    }
}

void FastPathEligibleInclusionNode::_applyProjectionsToArray(BSONObj array,
                                                             BSONArrayBuilder* bab) const {
    BSONObjIterator it{array};

    while (it.more()) {
        const auto bsonElement{it.next()};

        if (bsonElement.type() == BSONType::Object) {
            BSONObjBuilder subBob{bab->subobjStart()};
            _applyProjections(bsonElement.embeddedObject(), &subBob);
        } else if (bsonElement.type() == BSONType::Array) {
            if (_policies.arrayRecursionPolicy ==
                ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays) {
                continue;
            }
            BSONArrayBuilder subBab{bab->subarrayStart()};
            _applyProjectionsToArray(bsonElement.embeddedObject(), &subBab);
        } else {
            // The projection semantics dictate to drop scalar array elements when we're projecting
            // through an array path.
        }
    }
}
}  // namespace mongo::projection_executor
