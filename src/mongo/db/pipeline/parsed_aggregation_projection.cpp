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

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

#include <boost/optional.hpp>
#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/parsed_exclusion_projection.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

namespace {

using ComputedFieldsPolicy = ProjectionPolicies::ComputedFieldsPolicy;

std::string makeBannedComputedFieldsErrorMessage(BSONObj projSpec) {
    return str::stream() << "Bad projection specification, cannot use computed fields when parsing "
                            "a spec in kBanComputedFields mode: "
                         << projSpec.toString();
}

/**
 * This class is responsible for determining what type of $project stage it specifies.
 */
class ProjectTypeParser {
public:
    /**
     * Parses 'spec' to determine whether it is an inclusion or exclusion projection. 'Computed'
     * fields (ones which are defined by an expression or a literal) are treated as inclusion
     * projections for in this context of the $project stage.
     */
    static TransformerType parse(const BSONObj& spec, ProjectionPolicies policies) {
        ProjectTypeParser parser(spec, policies);
        parser.parse();
        invariant(parser._parsedType);
        return *(parser._parsedType);
    }

private:
    ProjectTypeParser(const BSONObj& spec, ProjectionPolicies policies)
        : _rawObj(spec), _policies(policies) {}

    /**
     * Traverses '_rawObj' to determine the type of projection, populating '_parsedType' in the
     * process.
     */
    void parse() {
        size_t nFields = 0;
        for (auto&& elem : _rawObj) {
            parseElement(elem, FieldPath(elem.fieldName()));
            nFields++;
        }

        // Check for the case where we only exclude '_id'.
        if (nFields == 1) {
            BSONElement elem = _rawObj.firstElement();
            if (elem.fieldNameStringData() == "_id" && (elem.isBoolean() || elem.isNumber()) &&
                !elem.trueValue()) {
                _parsedType = TransformerType::kExclusionProjection;
            }
        }

        // Default to inclusion if nothing (except maybe '_id') is explicitly included or excluded.
        if (!_parsedType) {
            _parsedType = TransformerInterface::TransformerType::kInclusionProjection;
        }
    }

    /**
     * Parses a single BSONElement. 'pathToElem' should include the field name of 'elem'.
     *
     * Delegates to parseSubObject() if 'elem' is an object. Otherwise updates '_parsedType' if
     * appropriate.
     *
     * Throws a AssertionException if this element represents a mix of projection types. If we are
     * parsing in ComputedFieldsPolicy::kBanComputedFields mode, an inclusion projection
     * which contains computed fields will also be rejected.
     */
    void parseElement(const BSONElement& elem, const FieldPath& pathToElem) {
        if (elem.type() == BSONType::Object) {
            return parseNestedObject(elem.Obj(), pathToElem);
        }

        // If this element is not a boolean or numeric value, then it is a literal value. These are
        // illegal if we are in kBanComputedFields parse mode.
        uassert(ErrorCodes::FailedToParse,
                makeBannedComputedFieldsErrorMessage(_rawObj),
                elem.isBoolean() || elem.isNumber() ||
                    _policies.computedFieldsPolicy != ComputedFieldsPolicy::kBanComputedFields);

        if (pathToElem.fullPath() == "_id") {
            // If the _id field is a computed value, then this must be an inclusion projection. If
            // it is numeric or boolean, then this does not determine the projection type, due to
            // the fact that inclusions may explicitly exclude _id and exclusions may include _id.
            if (!elem.isBoolean() && !elem.isNumber()) {
                uassert(ErrorCodes::FailedToParse,
                        str::stream() << "Bad projection specification, '_id' may not be a "
                                         "computed field in an exclusion projection: "
                                      << _rawObj.toString(),
                        !_parsedType ||
                            _parsedType ==
                                TransformerInterface::TransformerType::kInclusionProjection);
                _parsedType = TransformerInterface::TransformerType::kInclusionProjection;
            }
        } else if ((elem.isBoolean() || elem.isNumber()) && !elem.trueValue()) {
            // If this is an excluded field other than '_id', ensure that the projection type has
            // not already been set to kInclusionProjection.
            uassert(
                40178,
                str::stream() << "Bad projection specification, cannot exclude fields "
                                 "other than '_id' in an inclusion projection: "
                              << _rawObj.toString(),
                !_parsedType ||
                    (*_parsedType == TransformerInterface::TransformerType::kExclusionProjection));
            _parsedType = TransformerInterface::TransformerType::kExclusionProjection;
        } else {
            // A boolean true, a truthy numeric value, or any expression can only be used with an
            // inclusion projection. Note that literal values like "string" or null are also treated
            // as expressions.
            uassert(
                40179,
                str::stream() << "Bad projection specification, cannot include fields or "
                                 "add computed fields during an exclusion projection: "
                              << _rawObj.toString(),
                !_parsedType ||
                    (*_parsedType == TransformerInterface::TransformerType::kInclusionProjection));
            _parsedType = TransformerInterface::TransformerType::kInclusionProjection;
        }
    }

    /**
     * Traverses 'thisLevelSpec', parsing each element in turn.
     *
     * Throws a AssertionException if 'thisLevelSpec' represents an invalid mix of projections. If
     * we are parsing in ComputedFieldsPolicy::kBanComputedFields mode, an inclusion
     * projection which contains computed fields will also be rejected.
     */
    void parseNestedObject(const BSONObj& thisLevelSpec, const FieldPath& prefix) {

        for (auto&& elem : thisLevelSpec) {
            auto fieldName = elem.fieldNameStringData();
            if (fieldName[0] == '$') {
                // This object is an expression specification like {$add: [...]}. It will be parsed
                // into an Expression later, but for now, just track that the prefix has been
                // specified, validate that computed projections are legal, and skip it.
                uassert(ErrorCodes::FailedToParse,
                        makeBannedComputedFieldsErrorMessage(_rawObj),
                        _policies.computedFieldsPolicy != ComputedFieldsPolicy::kBanComputedFields);
                uassert(40182,
                        str::stream() << "Bad projection specification, cannot include fields or "
                                         "add computed fields during an exclusion projection: "
                                      << _rawObj.toString(),
                        !_parsedType ||
                            _parsedType ==
                                TransformerInterface::TransformerType::kInclusionProjection);
                _parsedType = TransformerInterface::TransformerType::kInclusionProjection;
                continue;
            }
            parseElement(elem, FieldPath::getFullyQualifiedPath(prefix.fullPath(), fieldName));
        }
    }

    // The original object. Used to generate more helpful error messages.
    const BSONObj& _rawObj;

    // This will be populated during parse().
    boost::optional<TransformerType> _parsedType;

    // Policies associated with the projection which determine its runtime behaviour.
    ProjectionPolicies _policies;
};

}  // namespace

std::unique_ptr<ParsedAggregationProjection> ParsedAggregationProjection::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& spec,
    ProjectionPolicies policies) {
    // Checks that the specification was valid, and throws if it is not.
    ProjectionSpecValidator::uassertValid(spec);
    // Check for any conflicting specifications, and determine the type of the projection.
    auto projectionType = ProjectTypeParser::parse(spec, policies);
    // kComputed is a projection type reserved for $addFields, and should never be detected by the
    // ProjectTypeParser.
    invariant(projectionType != TransformerType::kComputedProjection);

    // We can't use make_unique() here, since the branches have different types.
    std::unique_ptr<ParsedAggregationProjection> parsedProject(
        projectionType == TransformerType::kInclusionProjection
            ? static_cast<ParsedAggregationProjection*>(
                  new ParsedInclusionProjection(expCtx, policies))
            : static_cast<ParsedAggregationProjection*>(
                  new ParsedExclusionProjection(expCtx, policies)));

    // Actually parse the specification.
    parsedProject->parse(spec);
    return parsedProject;
}
}  // namespace parsed_aggregation_projection
}  // namespace mongo
