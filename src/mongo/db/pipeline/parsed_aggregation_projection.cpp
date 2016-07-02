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

#include <boost/optional.hpp>
#include <string>
#include <unordered_set>

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/parsed_exclusion_projection.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace parsed_aggregation_projection {

namespace {

/**
 * This class is responsible for determining if the provided specification is valid, and determining
 * whether it specifies an inclusion projection or an exclusion projection.
 */
class ProjectSpecTypeParser {
public:
    /**
     * Parses 'spec' to determine whether it is an inclusion or exclusion projection.
     *
     * Throws a UserException if the specification is invalid.
     */
    static ProjectionType parse(const BSONObj& spec) {
        ProjectSpecTypeParser parser(spec);
        parser.parse();
        invariant(parser._parsedType);
        return *(parser._parsedType);
    }

private:
    ProjectSpecTypeParser(const BSONObj& spec) : _rawObj(spec) {}

    /**
     * Uses '_seenPaths' to see if 'path' conflicts with any paths that have already been specified.
     *
     * For example, a user is not allowed to specify {'a': 1, 'a.b': 1}, or some similar conflicting
     * paths.
     */
    void ensurePathDoesNotConflictOrThrow(StringData path) {
        for (auto&& seenPath : _seenPaths) {
            uassert(40176,
                    str::stream() << "$project specification contains two conflicting paths. "
                                     "Cannot specify both '"
                                  << path
                                  << "' and '"
                                  << seenPath
                                  << "': "
                                  << _rawObj.toString(),
                    path != seenPath && !expression::isPathPrefixOf(path, seenPath) &&
                        !expression::isPathPrefixOf(seenPath, path));
        }
        _seenPaths.insert(path.toString());
    }

    /**
     * Traverses '_rawObj' to determine the type of projection, populating '_parsedType' in the
     * process.
     *
     * Throws a UserException if an invalid projection specification is detected.
     */
    void parse() {
        uassert(40177, "$project specification must have at least one field", !_rawObj.isEmpty());

        for (auto&& elem : _rawObj) {
            parseElement(elem, FieldPath(elem.fieldName()));
        }

        // Default to inclusion if nothing (except maybe '_id') is explicitly included or excluded.
        if (!_parsedType) {
            _parsedType = ProjectionType::kInclusion;
        }
    }

    /**
     * Parses a single BSONElement. 'pathToElem' should include the field name of 'elem'.
     *
     * Delegates to parseSubObject() if 'elem' is an object. Otherwise adds the full path to 'elem'
     * to '_seenPaths', and updates '_parsedType' if appropriate.
     *
     * Throws a UserException if the path to 'elem' conflicts with a path that has already been
     * specified, or if this element represents a mix of projection types.
     */
    void parseElement(const BSONElement& elem, const FieldPath& pathToElem) {
        if (elem.type() == BSONType::Object) {
            return parseNestedObject(elem.Obj(), pathToElem);
        }

        if ((elem.isBoolean() || elem.isNumber()) && !elem.trueValue()) {
            // A top-level exclusion of "_id" is allowed in either an inclusion projection or an
            // exclusion projection, so doesn't affect '_parsedType'.
            if (pathToElem.fullPath() != "_id") {
                uassert(40178,
                        str::stream() << "Bad projection specification, cannot exclude fields "
                                         "other than '_id' in an inclusion projection: "
                                      << _rawObj.toString(),
                        !_parsedType || (*_parsedType == ProjectionType::kExclusion));
                _parsedType = ProjectionType::kExclusion;
            }
        } else {
            // A boolean true, a truthy numeric value, or any expression can only be used with an
            // inclusion projection. Note that literal values like "string" or null are also treated
            // as expressions.
            uassert(40179,
                    str::stream() << "Bad projection specification, cannot include fields or "
                                     "add computed fields during an exclusion projection: "
                                  << _rawObj.toString(),
                    !_parsedType || (*_parsedType == ProjectionType::kInclusion));
            _parsedType = ProjectionType::kInclusion;
        }
        ensurePathDoesNotConflictOrThrow(pathToElem.fullPath());
    }

    /**
     * Traverses 'thisLevelSpec', parsing each element in turn.
     *
     * Throws a UserException if any paths conflict with each other or existing paths,
     * 'thisLevelSpec' contains a dotted path, or if 'thisLevelSpec' represents an invalid
     * expression.
     */
    void parseNestedObject(const BSONObj& thisLevelSpec, const FieldPath& prefix) {
        uassert(40180,
                str::stream() << "an empty object is not a valid value in a $project. Found "
                                 "empty object at path "
                              << prefix.fullPath(),
                !thisLevelSpec.isEmpty());

        for (auto elem : thisLevelSpec) {
            auto fieldName = elem.fieldNameStringData();
            if (fieldName[0] == '$') {
                // This object is an expression specification like {$add: [...]}. It will be parsed
                // into an Expression later, but for now, just track that the prefix has been
                // specified and skip it.
                uassert(40181,
                        str::stream()
                            << "Bad projection specification: An expression specification must "
                               "contain exactly one field, the name of the expression. Found "
                            << thisLevelSpec.nFields()
                            << " fields in "
                            << thisLevelSpec.toString()
                            << ", while parsing $project object "
                            << _rawObj.toString(),
                        thisLevelSpec.nFields() == 1);
                uassert(40182,
                        str::stream() << "Bad projection specification, cannot include fields or "
                                         "add computed fields during an exclusion projection: "
                                      << _rawObj.toString(),
                        !_parsedType || _parsedType == ProjectionType::kInclusion);
                _parsedType = ProjectionType::kInclusion;
                ensurePathDoesNotConflictOrThrow(prefix.fullPath());
                continue;
            }

            uassert(40183,
                    str::stream() << "cannot use dotted field name '" << fieldName
                                  << "' in a sub object of a $project stage: "
                                  << _rawObj.toString(),
                    fieldName.find('.') == std::string::npos);

            parseElement(elem, FieldPath::getFullyQualifiedPath(prefix.fullPath(), fieldName));
        }
    }

    // The original object. Used to generate more helpful error messages.
    const BSONObj& _rawObj;

    // This will be populated during parse().
    boost::optional<ProjectionType> _parsedType;

    // Tracks which paths we've seen to ensure no two paths conflict with each other.
    std::unordered_set<std::string> _seenPaths;
};

}  // namespace

std::unique_ptr<ParsedAggregationProjection> ParsedAggregationProjection::create(
    const BSONObj& spec) {
    // Check for any conflicting specifications, and determine the type of the projection.
    auto projectionType = ProjectSpecTypeParser::parse(spec);

    // We can't use make_unique() here, since the branches have different types.
    std::unique_ptr<ParsedAggregationProjection> parsedProject(
        projectionType == ProjectionType::kInclusion
            ? static_cast<ParsedAggregationProjection*>(new ParsedInclusionProjection())
            : static_cast<ParsedAggregationProjection*>(new ParsedExclusionProjection()));

    // Actually parse the specification.
    parsedProject->parse(spec);
    return parsedProject;
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
