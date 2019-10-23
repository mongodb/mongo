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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_exclusion_projection.h"

#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

namespace parsed_aggregation_projection {

Document ParsedExclusionProjection::serializeTransformation(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return _root->serialize(explain);
}

Document ParsedExclusionProjection::applyProjection(const Document& inputDoc) const {
    return _root->applyToDocument(inputDoc);
}

void ParsedExclusionProjection::parse(const BSONObj& spec, ExclusionNode* node, size_t depth) {
    bool idSpecified = false;

    for (auto elem : spec) {
        const auto fieldName = elem.fieldNameStringData();

        // Track whether the projection spec specifies a desired behavior for the _id field.
        idSpecified = idSpecified || fieldName == "_id"_sd || fieldName.startsWith("_id."_sd);

        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // We have already verified this is an exclusion projection. _id is the only field
                // which is permitted to be explicitly included here.
                invariant(!elem.trueValue() || elem.fieldNameStringData() == "_id"_sd);
                if (!elem.trueValue()) {
                    node->addProjectionForPath(FieldPath(fieldName));
                }
                break;
            }
            case BSONType::Object: {
                // This object represents a nested projection specification, like the sub-object in
                // {a: {b: 0, c: 0}} or {"a.b": {c: 0}}.
                ExclusionNode* child;

                if (elem.fieldNameStringData().find('.') == std::string::npos) {
                    child = node->addOrGetChild(fieldName.toString());
                } else {
                    // A dotted field is not allowed in a sub-object, and should have been detected
                    // in ParsedAggregationProjection's parsing before we get here.
                    invariant(depth == 0);

                    // We need to keep adding children to our tree until we create a child that
                    // represents this dotted path.
                    child = node;
                    auto fullPath = FieldPath(fieldName);
                    while (fullPath.getPathLength() > 1) {
                        child = child->addOrGetChild(fullPath.getFieldName(0).toString());
                        fullPath = fullPath.tail();
                    }
                    // It is illegal to construct an empty FieldPath, so the above loop ends one
                    // iteration too soon. Add the last path here.
                    child = child->addOrGetChild(fullPath.fullPath());
                }

                parse(elem.Obj(), child, depth + 1);
                break;
            }
            default: { MONGO_UNREACHABLE; }
        }
    }

    // If _id was not specified, then doing nothing will cause it to be included. If the default _id
    // policy is kExcludeId, we add a new entry for _id to the ExclusionNode tree here.
    if (!idSpecified && _policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kExcludeId) {
        _root->addProjectionForPath({FieldPath("_id")});
    }
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
