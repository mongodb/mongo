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

#include "mongo/db/exec/add_fields_projection_executor.h"

#include <algorithm>

#include "mongo/db/matcher/expression_algo.h"

namespace mongo::projection_executor {
namespace {
using TransformerType = TransformerInterface::TransformerType;
using expression::isPathPrefixOf;

/**
 * This class ensures that the specification was valid: that none of the paths specified conflict
 * with one another, that there is at least one field, etc. Here "projection" includes $addFields
 * specifications.
 */
class ProjectionSpecValidator {
public:
    /**
     * Throws if the specification is not valid for a projection. Because this validator is meant to
     * be generic, the error thrown is generic.  Callers at the DocumentSource level should modify
     * the error message if they want to include information specific to the stage name used.
     */
    static void uassertValid(const BSONObj& spec);

private:
    ProjectionSpecValidator(const BSONObj& spec) : _rawObj(spec) {}

    /**
     * Uses '_seenPaths' to see if 'path' conflicts with any paths that have already been specified.
     *
     * For example, a user is not allowed to specify {'a': 1, 'a.b': 1}, or some similar conflicting
     * paths.
     */
    void ensurePathDoesNotConflictOrThrow(const std::string& path);

    /**
     * Throws if an invalid projection specification is detected.
     */
    void validate();

    /**
     * Parses a single BSONElement. 'pathToElem' should include the field name of 'elem'.
     *
     * Delegates to parseSubObject() if 'elem' is an object. Otherwise adds the full path to 'elem'
     * to '_seenPaths'.
     *
     * Calls ensurePathDoesNotConflictOrThrow with the path to this element, throws on conflicting
     * path specifications.
     */
    void parseElement(const BSONElement& elem, const FieldPath& pathToElem);

    /**
     * Traverses 'thisLevelSpec', parsing each element in turn.
     *
     * Throws if any paths conflict with each other or existing paths, 'thisLevelSpec' contains a
     * dotted path, or if 'thisLevelSpec' represents an invalid expression.
     */
    void parseNestedObject(const BSONObj& thisLevelSpec, const FieldPath& prefix);

    // The original object. Used to generate more helpful error messages.
    const BSONObj& _rawObj;

    // Tracks which paths we've seen to ensure no two paths conflict with each other.
    std::set<std::string, PathPrefixComparator> _seenPaths;
};

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
}  // namespace

std::unique_ptr<AddFieldsProjectionExecutor> AddFieldsProjectionExecutor::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& spec) {
    // Verify that we don't have conflicting field paths, etc.
    ProjectionSpecValidator::uassertValid(spec);
    auto executor = std::make_unique<AddFieldsProjectionExecutor>(expCtx);

    // Actually parse the specification.
    executor->parse(spec);
    return executor;
}

std::unique_ptr<AddFieldsProjectionExecutor> AddFieldsProjectionExecutor::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const FieldPath& fieldPath,
    const boost::intrusive_ptr<Expression>& expr) {

    // This helper is only meant for creating top-level fields. Dotted field paths require
    // thinking about implicit array traversal.
    tassert(5339700,
            str::stream() << "Expected a top-level field name, but got " << fieldPath.fullPath(),
            fieldPath.getPathLength() == 1);

    auto executor = std::make_unique<AddFieldsProjectionExecutor>(expCtx);
    executor->_root->addExpressionForPath(fieldPath, expr);
    return executor;
}

void AddFieldsProjectionExecutor::parse(const BSONObj& spec) {
    for (auto elem : spec) {
        // The field name might be a dotted path.
        auto fieldPath = FieldPath(elem.fieldNameStringData());

        if (elem.type() == BSONType::Object) {
            // This is either an expression, or a nested specification.
            if (parseObjectAsExpression(fieldPath, elem.Obj(), _expCtx->variablesParseState)) {
                // It was an expression.
            } else {
                parseSubObject(elem.Obj(), _expCtx->variablesParseState, fieldPath);
            }
        } else {
            // This is a literal or regular value.
            _root->addExpressionForPath(
                fieldPath,
                Expression::parseOperand(_expCtx.get(), elem, _expCtx->variablesParseState));
        }
    }
}

Document AddFieldsProjectionExecutor::applyProjection(const Document& inputDoc) const {
    // The output doc is the same as the input doc, with the added fields.
    MutableDocument output(inputDoc);
    _root->applyExpressions(inputDoc, &output);

    // Pass through the metadata.
    output.copyMetaDataFrom(inputDoc);
    return output.freeze();
}

bool AddFieldsProjectionExecutor::parseObjectAsExpression(
    const FieldPath& pathToObject,
    const BSONObj& objSpec,
    const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We already verified that it has only one field.
        invariant(objSpec.nFields() == 1);
        _root->addExpressionForPath(
            pathToObject, Expression::parseExpression(_expCtx.get(), objSpec, variablesParseState));
        return true;
    }
    return false;
}

void AddFieldsProjectionExecutor::parseSubObject(const BSONObj& subObj,
                                                 const VariablesParseState& variablesParseState,
                                                 const FieldPath& pathToObj) {
    bool elemInSubObj = false;
    for (auto&& elem : subObj) {
        elemInSubObj = true;
        auto fieldName = elem.fieldNameStringData();
        invariant(fieldName[0] != '$');
        // Dotted paths in a sub-object have already been detected and disallowed by the function
        // ProjectionSpecValidator::validate().
        invariant(fieldName.find('.') == std::string::npos);

        auto currentPath = pathToObj.concat(fieldName);
        if (elem.type() == BSONType::Object) {
            // This is either an expression, or a nested specification.
            if (!parseObjectAsExpression(currentPath, elem.Obj(), variablesParseState)) {
                // It was a nested subobject
                parseSubObject(elem.Obj(), variablesParseState, currentPath);
            }
        } else {
            // This is a literal or regular value.
            _root->addExpressionForPath(
                currentPath, Expression::parseOperand(_expCtx.get(), elem, variablesParseState));
        }
    }

    if (!elemInSubObj) {
        _root->addExpressionForPath(
            pathToObj, Expression::parseObject(_expCtx.get(), subObj, variablesParseState));
    }
}
}  // namespace mongo::projection_executor
