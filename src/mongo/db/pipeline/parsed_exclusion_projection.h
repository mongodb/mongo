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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

namespace mongo {

class FieldPath;
class Value;

namespace parsed_aggregation_projection {

/**
 * A node used to define the parsed structure of an exclusion projection. Each ExclusionNode
 * represents one 'level' of the parsed specification. The root ExclusionNode represents all top
 * level exclusions, with any child ExclusionNodes representing dotted or nested exclusions.
 */
class ExclusionNode {
public:
    ExclusionNode(std::string pathToNode = "");

    /**
     * Serialize this exclusion.
     */
    Document serialize() const;

    /**
     * Mark this path to be excluded. 'path' is allowed to be dotted.
     */
    void excludePath(FieldPath path);

    /**
     * Applies this tree of exclusions to the input document.
     */
    Document applyProjection(Document input) const;

    /**
     * Creates the child if it doesn't already exist. 'field' is not allowed to be dotted.
     */
    ExclusionNode* addOrGetChild(FieldPath field);


private:
    // Helpers for addOrGetChild above.
    ExclusionNode* getChild(std::string field) const;
    ExclusionNode* addChild(std::string field);

    // Helper for applyProjection above.
    Value applyProjectionToValue(Value val) const;

    // Fields excluded at this level.
    std::unordered_set<std::string> _excludedFields;

    std::string _pathToNode;
    std::unordered_map<std::string, std::unique_ptr<ExclusionNode>> _children;
};

/**
 * A ParsedExclusionProjection represents a parsed form of the raw BSON specification.
 *
 * This class is mostly a wrapper around an ExclusionNode tree. It contains logic to parse a
 * specification object into the corresponding ExclusionNode tree, but defers most execution logic
 * to the underlying tree.
 */
class ParsedExclusionProjection : public ParsedAggregationProjection {
public:
    ParsedExclusionProjection() : ParsedAggregationProjection(), _root(new ExclusionNode()) {}

    ProjectionType getType() const final {
        return ProjectionType::kExclusion;
    }

    Document serialize(bool explain = false) const final;

    /**
     * Parses the projection specification given by 'spec', populating internal data structures.
     */
    void parse(const BSONObj& spec) final {
        parse(spec, _root.get(), 0);
    }

    /**
     * Exclude the fields specified.
     */
    Document applyProjection(Document inputDoc) const final;

private:
    /**
     * Helper for parse() above.
     *
     * Traverses 'spec' and parses each field. Adds any excluded fields at this level to 'node',
     * and recurses on any sub-objects.
     */
    void parse(const BSONObj& spec, ExclusionNode* node, size_t depth);


    // The ExclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<ExclusionNode> _root;
};

}  // namespace parsed_aggregation_projection
}  // namespace mongo
