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
#include <unordered_map>
#include <unordered_set>

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class FieldPath;
class Value;

namespace parsed_aggregation_projection {

/**
 * A node used to define the parsed structure of an inclusion projection. Each InclusionNode
 * represents one 'level' of the parsed specification. The root InclusionNode represents all top
 * level inclusions or additions, with any child InclusionNodes representing dotted or nested
 * inclusions or additions.
 */
class InclusionNode {
public:
    InclusionNode(std::string pathToNode = "");

    /**
     * Optimize any computed expressions.
     */
    void optimize();

    /**
     * Serialize this projection.
     */
    void serialize(MutableDocument* output, bool explain) const;

    /**
     * Adds dependencies of any fields that need to be included, or that are used by any
     * expressions.
     */
    void addDependencies(DepsTracker* deps) const;

    /**
     * Loops over 'inputDoc', extracting and appending any included fields into 'outputDoc'. This
     * will also copy over enough information to preserve the structure of the incoming document for
     * all the fields this projection cares about.
     *
     * For example, given an InclusionNode tree representing this projection:
     *   {a: {b: 1, c: <exp>}, "d.e": <exp>}
     * calling applyInclusions() with an 'inputDoc' of
     *   {a: [{b: 1, d: 1}, {b: 2, d: 2}], d: [{e: 1, f: 1}, {e: 1, f: 1}]}
     * and an empty 'outputDoc' will leave 'outputDoc' representing the document
     *   {a: [{b: 1}, {b: 2}], d: [{}, {}]}.
     */
    void applyInclusions(Document inputDoc, MutableDocument* outputDoc) const;

    /**
     * Add computed fields to 'outputDoc'. 'vars' is passed through to be used in Expression
     * evaluation.
     */
    void addComputedFields(MutableDocument* outputDoc, Variables* vars) const;

    /**
     * Creates the child if it doesn't already exist. 'field' is not allowed to be dotted.
     */
    InclusionNode* addOrGetChild(std::string field);

    /**
     * Recursively adds 'path' into the tree as a computed field, creating any child nodes if
     * necessary.
     *
     * 'path' is allowed to be dotted, and is assumed not to conflict with another path already in
     * the tree. For example, it is an error to add the path "a.b" as a computed field to a tree
     * which has already included the field "a".
     */
    void addComputedField(const FieldPath& path, boost::intrusive_ptr<Expression> expr);

    /**
     * Recursively adds 'path' into the tree as an included field, creating any child nodes if
     * necessary.
     *
     * 'path' is allowed to be dotted, and is assumed not to conflict with another path already in
     * the tree. For example, it is an error to include the path "a.b" from a tree which has already
     * added a computed field "a".
     */
    void addIncludedField(const FieldPath& path);

    std::string getPath() const {
        return _pathToNode;
    }

    void injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    // Helpers for the Document versions above. These will apply the transformation recursively to
    // each element of any arrays, and ensure non-documents are handled appropriately.
    Value applyInclusionsToValue(Value inputVal) const;
    Value addComputedFields(Value inputVal, Variables* vars) const;

    /**
     * Returns nullptr if no such child exists.
     */
    InclusionNode* getChild(std::string field) const;

    /**
     * Adds a new InclusionNode as a child. 'field' cannot be dotted.
     */
    InclusionNode* addChild(std::string field);

    /**
     * Returns true if this node or any child of this node contains a computed field.
     */
    bool subtreeContainsComputedFields() const;

    std::string _pathToNode;

    // Our projection semantics are such that all field additions need to be processed in the order
    // specified. '_orderToProcessAdditionsAndChildren' tracks that order.
    //
    // For example, for the specification {a: <expression>, "b.c": <expression>, d: <expression>},
    // we need to add the top level fields in the order "a", then "b", then "d". This ordering
    // information needs to be tracked separately, since "a" and "d" will be tracked via
    // '_expressions', and "b.c" will be tracked as a child InclusionNode in '_children'. For the
    // example above, '_orderToProcessAdditionsAndChildren' would be ["a", "b", "d"].
    std::vector<std::string> _orderToProcessAdditionsAndChildren;

    StringMap<boost::intrusive_ptr<Expression>> _expressions;
    std::unordered_set<std::string> _inclusions;

    // TODO use StringMap once SERVER-23700 is resolved.
    std::unordered_map<std::string, std::unique_ptr<InclusionNode>> _children;
};

/**
 * A ParsedInclusionProjection represents a parsed form of the raw BSON specification.
 *
 * This class is mostly a wrapper around an InclusionNode tree. It contains logic to parse a
 * specification object into the corresponding InclusionNode tree, but defers most execution logic
 * to the underlying tree.
 */
class ParsedInclusionProjection : public ParsedAggregationProjection {
public:
    ParsedInclusionProjection() : ParsedAggregationProjection(), _root(new InclusionNode()) {}

    ProjectionType getType() const final {
        return ProjectionType::kInclusion;
    }

    /**
     * Parses the projection specification given by 'spec', populating internal data structures.
     */
    void parse(const BSONObj& spec) final {
        VariablesIdGenerator idGenerator;
        VariablesParseState variablesParseState(&idGenerator);
        parse(spec, variablesParseState);
        _variables = stdx::make_unique<Variables>(idGenerator.getIdCount());
    }

    /**
     * Serialize the projection.
     */
    Document serialize(bool explain = false) const final {
        MutableDocument output;
        if (_idExcluded) {
            output.addField("_id", Value(false));
        }
        _root->serialize(&output, explain);
        return output.freeze();
    }

    /**
     * Optimize any computed expressions.
     */
    void optimize() final {
        _root->optimize();
    }

    void injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx) final {
        _root->injectExpressionContext(expCtx);
    }

    void addDependencies(DepsTracker* deps) const final {
        _root->addDependencies(deps);
    }

    /**
     * Apply this exclusion projection to 'inputDoc'.
     *
     * All inclusions are processed before all computed fields. Computed fields will be added
     * afterwards in the order in which they were specified to the $project stage.
     *
     * Arrays will be traversed, with any dotted/nested exclusions or computed fields applied to
     * each element in the array.
     */
    Document applyProjection(Document inputDoc) const final {
        _variables->setRoot(inputDoc);
        return applyProjection(inputDoc, _variables.get());
    }

    Document applyProjection(Document inputDoc, Variables* vars) const;

private:
    /**
     * Parses 'spec' to determine which fields to include, which are computed, and whether to
     * include '_id' or not.
     */
    void parse(const BSONObj& spec, const VariablesParseState& variablesParseState);

    /**
     * Attempts to parse 'objSpec' as an expression like {$add: [...]}. Adds a computed field to
     * '_root' and returns true if it was successfully parsed as an expression. Returns false if it
     * was not an expression specification.
     *
     * Throws an error if it was determined to be an expression specification, but failed to parse
     * as a valid expression.
     */
    bool parseObjectAsExpression(StringData pathToObject,
                                 const BSONObj& objSpec,
                                 const VariablesParseState& variablesParseState);

    /**
     * Traverses 'subObj' and parses each field. Adds any included or computed fields at this level
     * to 'node'.
     */
    void parseSubObject(const BSONObj& subObj,
                        const VariablesParseState& variablesParseState,
                        InclusionNode* node);

    // Not strictly necessary to track here, but makes serialization easier.
    bool _idExcluded = false;

    // The InclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<InclusionNode> _root;

    // This is needed to give the expressions knowledge about the context in which they are being
    // executed.
    std::unique_ptr<Variables> _variables;
};
}  // namespace parsed_aggregation_projection
}  // namespace mongo
