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

#include "mongo/db/pipeline/parsed_add_fields.h"

#include <algorithm>

#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"

namespace mongo {
namespace parsed_aggregation_projection {

using TransformerType = TransformerInterface::TransformerType;

using expression::isPathPrefixOf;

//
// ProjectionSpecValidator
//

void ProjectionSpecValidator::uassertValid(const BSONObj& spec) {
    ProjectionSpecValidator(spec).validate();
}

void ProjectionSpecValidator::ensurePathDoesNotConflictOrThrow(const std::string& path) {
    auto result = _seenPaths.emplace(path);
    auto pos = result.first;

    // Check whether the path was a duplicate of an existing path.
    auto conflictingPath = boost::make_optional(!result.second, *pos);

    // Check whether the preceding path prefixes this path.
    if (!conflictingPath && pos != _seenPaths.begin()) {
        conflictingPath =
            boost::make_optional(isPathPrefixOf(*std::prev(pos), path), *std::prev(pos));
    }

    // Check whether this path prefixes the subsequent path.
    if (!conflictingPath && std::next(pos) != _seenPaths.end()) {
        conflictingPath =
            boost::make_optional(isPathPrefixOf(path, *std::next(pos)), *std::next(pos));
    }

    uassert(40176,
            str::stream() << "specification contains two conflicting paths. "
                             "Cannot specify both '"
                          << path << "' and '" << *conflictingPath << "': " << _rawObj.toString(),
            !conflictingPath);
}

void ProjectionSpecValidator::validate() {
    if (_rawObj.isEmpty()) {
        uasserted(40177, "specification must have at least one field");
    }
    for (auto&& elem : _rawObj) {
        parseElement(elem, FieldPath(elem.fieldName()));
    }
}

void ProjectionSpecValidator::parseElement(const BSONElement& elem, const FieldPath& pathToElem) {
    if (elem.type() == BSONType::Object) {
        parseNestedObject(elem.Obj(), pathToElem);
    } else {
        ensurePathDoesNotConflictOrThrow(pathToElem.fullPath());
    }
}

void ProjectionSpecValidator::parseNestedObject(const BSONObj& thisLevelSpec,
                                                const FieldPath& prefix) {
    if (thisLevelSpec.isEmpty()) {
        uasserted(
            40180,
            str::stream() << "an empty object is not a valid value. Found empty object at path "
                          << prefix.fullPath());
    }
    for (auto&& elem : thisLevelSpec) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName[0] == '$') {
            // This object is an expression specification like {$add: [...]}. It will be parsed
            // into an Expression later, but for now, just track that the prefix has been
            // specified and skip it.
            if (thisLevelSpec.nFields() != 1) {
                uasserted(40181,
                          str::stream() << "an expression specification must contain exactly "
                                           "one field, the name of the expression. Found "
                                        << thisLevelSpec.nFields() << " fields in "
                                        << thisLevelSpec.toString() << ", while parsing object "
                                        << _rawObj.toString());
            }
            ensurePathDoesNotConflictOrThrow(prefix.fullPath());
            continue;
        }
        if (fieldName.find('.') != std::string::npos) {
            uasserted(40183,
                      str::stream() << "cannot use dotted field name '" << fieldName
                                    << "' in a sub object: " << _rawObj.toString());
        }
        parseElement(elem, FieldPath::getFullyQualifiedPath(prefix.fullPath(), fieldName));
    }
}

std::unique_ptr<ParsedAddFields> ParsedAddFields::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& spec) {
    // Verify that we don't have conflicting field paths, etc.
    ProjectionSpecValidator::uassertValid(spec);
    std::unique_ptr<ParsedAddFields> parsedAddFields = std::make_unique<ParsedAddFields>(expCtx);

    // Actually parse the specification.
    parsedAddFields->parse(spec);
    return parsedAddFields;
}

void ParsedAddFields::parse(const BSONObj& spec) {
    for (auto elem : spec) {
        auto fieldName = elem.fieldNameStringData();

        if (elem.type() == BSONType::Object) {
            // This is either an expression, or a nested specification.
            if (parseObjectAsExpression(fieldName, elem.Obj(), _expCtx->variablesParseState)) {
                // It was an expression.
            } else {
                // The field name might be a dotted path. If so, we need to keep adding children
                // to our tree until we create a child that represents that path.
                auto remainingPath = FieldPath(elem.fieldName());
                auto* child = _root.get();
                while (remainingPath.getPathLength() > 1) {
                    child = child->addOrGetChild(remainingPath.getFieldName(0).toString());
                    remainingPath = remainingPath.tail();
                }
                // It is illegal to construct an empty FieldPath, so the above loop ends one
                // iteration too soon. Add the last path here.
                child = child->addOrGetChild(remainingPath.fullPath());
                parseSubObject(elem.Obj(), _expCtx->variablesParseState, child);
            }
        } else {
            // This is a literal or regular value.
            _root->addExpressionForPath(
                FieldPath(elem.fieldName()),
                Expression::parseOperand(_expCtx, elem, _expCtx->variablesParseState));
        }
    }
}

Document ParsedAddFields::applyProjection(const Document& inputDoc) const {
    // The output doc is the same as the input doc, with the added fields.
    MutableDocument output(inputDoc);
    _root->applyExpressions(inputDoc, &output);

    // Pass through the metadata.
    output.copyMetaDataFrom(inputDoc);
    return output.freeze();
}

bool ParsedAddFields::parseObjectAsExpression(StringData pathToObject,
                                              const BSONObj& objSpec,
                                              const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We already verified that it has only one field.
        invariant(objSpec.nFields() == 1);
        _root->addExpressionForPath(
            pathToObject, Expression::parseExpression(_expCtx, objSpec, variablesParseState));
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
                auto* child = node->addOrGetChild(fieldName);
                parseSubObject(elem.Obj(), variablesParseState, child);
            }
        } else {
            // This is a literal or regular value.
            node->addExpressionForPath(
                FieldPath(elem.fieldName()),
                Expression::parseOperand(_expCtx, elem, variablesParseState));
        }
    }
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
