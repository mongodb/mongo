/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_add_fields.h"

#include <algorithm>

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

namespace mongo {

namespace parsed_aggregation_projection {

std::unique_ptr<ParsedAddFields> ParsedAddFields::create(const BSONObj& spec) {
    // Verify that we don't have conflicting field paths, etc.
    Status status = ProjectionSpecValidator::validate(spec);
    if (!status.isOK()) {
        uasserted(status.location(),
                  str::stream() << "Invalid $addFields specification: " << status.reason());
    }
    std::unique_ptr<ParsedAddFields> parsedAddFields = stdx::make_unique<ParsedAddFields>();

    // Actually parse the specification.
    parsedAddFields->parse(spec);
    return parsedAddFields;
}

void ParsedAddFields::parse(const BSONObj& spec, const VariablesParseState& variablesParseState) {
    for (auto elem : spec) {
        auto fieldName = elem.fieldNameStringData();

        if (elem.type() == BSONType::Object) {
            // This is either an expression, or a nested specification.
            if (parseObjectAsExpression(fieldName, elem.Obj(), variablesParseState)) {
                // It was an expression.
            } else {
                // The field name might be a dotted path. If so, we need to keep adding children
                // to our tree until we create a child that represents that path.
                auto remainingPath = FieldPath(elem.fieldName());
                auto child = _root.get();
                while (remainingPath.getPathLength() > 1) {
                    child = child->addOrGetChild(remainingPath.getFieldName(0).toString());
                    remainingPath = remainingPath.tail();
                }
                // It is illegal to construct an empty FieldPath, so the above loop ends one
                // iteration too soon. Add the last path here.
                child = child->addOrGetChild(remainingPath.fullPath());
                parseSubObject(elem.Obj(), variablesParseState, child);
            }
        } else {
            // This is a literal or regular value.
            _root->addComputedField(FieldPath(elem.fieldName()),
                                    Expression::parseOperand(elem, variablesParseState));
        }
    }
}

Document ParsedAddFields::applyProjection(Document inputDoc, Variables* vars) const {
    // All expressions will be evaluated in the context of the input document, before any
    // transformations have been applied.
    vars->setRoot(inputDoc);

    // The output doc is the same as the input doc, with the added fields.
    MutableDocument output(inputDoc);
    _root->addComputedFields(&output, vars);

    // Pass through the metadata.
    output.copyMetaDataFrom(inputDoc);
    return output.freeze();
}

bool ParsedAddFields::parseObjectAsExpression(StringData pathToObject,
                                              const BSONObj& objSpec,
                                              const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We have already verified that it has only one
        // field.
        invariant(objSpec.nFields() == 1);
        _root->addComputedField(pathToObject,
                                Expression::parseExpression(objSpec, variablesParseState));
        return true;
    }
    return false;
}

void ParsedAddFields::parseSubObject(const BSONObj& subObj,
                                     const VariablesParseState& variablesParseState,
                                     InclusionNode* node) {
    for (auto&& elem : subObj) {
        invariant(elem.fieldName()[0] != '$');
        // Dotted paths in a sub-object have already been detected and disallowed by the function
        // ProjectionSpecValidator::validate().
        invariant(elem.fieldNameStringData().find('.') == std::string::npos);

        if (elem.type() == BSONType::Object) {
            // This is either an expression, or a nested specification.
            auto fieldName = elem.fieldNameStringData().toString();
            if (!parseObjectAsExpression(
                    FieldPath::getFullyQualifiedPath(node->getPath(), fieldName),
                    elem.Obj(),
                    variablesParseState)) {
                // It was a nested subobject
                auto child = node->addOrGetChild(fieldName);
                parseSubObject(elem.Obj(), variablesParseState, child);
            }
        } else {
            // This is a literal or regular value.
            node->addComputedField(FieldPath(elem.fieldName()),
                                   Expression::parseOperand(elem, variablesParseState));
        }
    }
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
