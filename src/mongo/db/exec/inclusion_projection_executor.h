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

#pragma once

#include <memory>

#include "mongo/db/exec/fastpath_projection_node.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_node.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/expression_walker.h"

namespace mongo::projection_executor {
/**
 * A node used to define the parsed structure of an inclusion projection. Each InclusionNode
 * represents one 'level' of the parsed specification. The root InclusionNode represents all top
 * level inclusions or additions, with any child InclusionNodes representing dotted or nested
 * inclusions or additions.
 */
class InclusionNode : public ProjectionNode {
public:
    InclusionNode(ProjectionPolicies policies, std::string pathToNode = "")
        : ProjectionNode(policies, std::move(pathToNode)) {}

    InclusionNode* addOrGetChild(const std::string& field) {
        return static_cast<InclusionNode*>(ProjectionNode::addOrGetChild(field));
    }

    void reportDependencies(DepsTracker* deps) const final {
        for (auto&& includedField : _projectedFields) {
            deps->fields.insert(FieldPath::getFullyQualifiedPath(_pathToNode, includedField));
        }

        if (!_pathToNode.empty() && _subtreeContainsComputedFields) {
            // The shape of any computed fields in the output will change depending on if there are
            // any arrays on the path to the expression.  In addition to any dependencies of the
            // expression itself, we need to add this field to our dependencies.
            deps->fields.insert(_pathToNode);
        }

        for (auto&& expressionPair : _expressions) {
            expression::addDependencies(expressionPair.second.get(), deps);
        }

        for (auto&& childPair : _children) {
            childPair.second->reportDependencies(deps);
        }
    }

    boost::optional<size_t> maxFieldsToProject() const override {
        return _children.size() + _projectedFields.size();
    }

    // The following two methods extract from the InclusionNode computed projections that depend
    // only on the 'oldName' field. We need two versions for $project and $addFields, due to
    // different functionality: we need to replace the fields in $project to not lose them, and we
    // can just remove them from $addFields.
    /**
     * Returns a pair of <BSONObj, bool>. The BSONObj contains extracted computed projections that
     * depend only on the 'oldName' field and is empty if no such projection exists. In the
     * extracted expressions the 'oldName' is substituted for the 'newName'. If a projection name
     * is in the 'reservedNames' set, it is ineligible for extraction. Each extracted computed
     * projection is replaced with a projected field or with an identity projection.
     * The returned boolean flag is always false meaning that the original projection is not empty
     * and cannot be deleted.
     */
    std::pair<BSONObj, bool> extractComputedProjectionsInProject(
        const StringData& oldName,
        const StringData& newName,
        const std::set<StringData>& reservedNames);

    /**
     * Returns a pair of <BSONObj, bool>. The BSONObj contains extracted computed projections that
     * depend only on the 'oldName' field and is empty if no such projection exists. In the
     * extracted expressions the 'oldName' is substituted for the 'newName'. If a projection name
     * is in the 'reservedNames' set, it is ineligible for extraction. To preserve the original
     * field order the extraction stops when reaching a field which cannot be extracted. The
     * extracted projections are removed from the node.
     * The returned boolean flag is true if the original projection has become empty after the
     * extraction and can be deleted by the caller.
     */
    std::pair<BSONObj, bool> extractComputedProjectionsInAddFields(
        const StringData& oldName,
        const StringData& newName,
        const std::set<StringData>& reservedNames);

protected:
    // For inclusions, we can apply an optimization here by simply appending to the output document
    // via MutableDocument::addField, rather than always checking for existing fields via setField.
    void outputProjectedField(StringData field, Value val, MutableDocument* outputDoc) const final {
        outputDoc->addField(field, val);
    }
    std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const override {
        return std::make_unique<InclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }
    MutableDocument initializeOutputDocument(const Document& inputDoc) const final {
        // Technically this value could be min(number of projected fields, size of input
        // document). However, the size() function on Document() can take linear time, so we just
        // allocate the number of projected fields.
        const auto maxPossibleResultingFields =
            _children.size() + _expressions.size() + _projectedFields.size();
        return MutableDocument{maxPossibleResultingFields};
    }
    Value applyLeafProjectionToValue(const Value& value) const final {
        return value;
    }
    Value transformSkippedValueForOutput(const Value& value) const final {
        return Value();
    }
};

/**
 * A fast-path inclusion projection implementation which applies a BSON-to-BSON transformation
 * rather than constructing an output document using the Document/Value API. For inclusion-only
 * projections (as defined by projection_ast::Projection::isInclusionOnly) it can be much faster
 * than the default InclusionNode implementation. On a document-by-document basis, if the fast-path
 * projection cannot be applied to the input document, it will fall back to the default
 * implementation.
 */
class FastPathEligibleInclusionNode final
    : public FastPathProjectionNode<FastPathEligibleInclusionNode, InclusionNode> {
private:
    using Base = FastPathProjectionNode<FastPathEligibleInclusionNode, InclusionNode>;

public:
    using Base::Base;

    Document applyToDocument(const Document& inputDoc) const final;

protected:
    std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const final {
        return std::make_unique<FastPathEligibleInclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }

private:
    void _applyToProjectedField(const BSONElement& element, BSONObjBuilder* bob) const {
        // This element is included by the projection, so it is added to the output.
        bob->append(element);
    }
    void _applyToNonProjectedField(const BSONElement& element, BSONObjBuilder* bob) const {
        // No-op. This element is not included in the projection, so it is not added to the output.
    }
    void _applyToNonProjectedField(const BSONElement& element, BSONArrayBuilder* bab) const {
        // No-op. This array element is not included in the projection, so it is not added to the
        // output.
    }
    void _applyToRemainingFields(BSONObjIterator& it, BSONObjBuilder* bob) const {
        // No-op. We processed all inclusions, rest of the elements can be discarded.
    }

    friend class FastPathProjectionNode<FastPathEligibleInclusionNode, InclusionNode>;
};

/**
 * A InclusionProjectionExecutor represents an execution tree for an inclusion projection.
 *
 * This class is mostly a wrapper around an InclusionNode tree and defers most execution logic to
 * the underlying tree.
 */
class InclusionProjectionExecutor : public ProjectionExecutor {
public:
    InclusionProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                ProjectionPolicies policies,
                                std::unique_ptr<InclusionNode> root)
        : ProjectionExecutor(expCtx, policies), _root(std::move(root)) {}

    InclusionProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                ProjectionPolicies policies,
                                bool allowFastPath = false)
        : InclusionProjectionExecutor(
              expCtx,
              policies,
              allowFastPath ? std::make_unique<FastPathEligibleInclusionNode>(policies)
                            : std::make_unique<InclusionNode>(policies)) {}

    TransformerType getType() const final {
        return TransformerType::kInclusionProjection;
    }

    const InclusionNode* getRoot() const {
        return _root.get();
    }

    InclusionNode* getRoot() {
        return _root.get();
    }

    /**
     * Serialize the projection.
     */
    Document serializeTransformation(boost::optional<ExplainOptions::Verbosity> explain,
                                     SerializationOptions options = {}) const final {
        MutableDocument output;

        // The InclusionNode tree in '_root' will always have a top-level _id node if _id is to be
        // included. If the _id node is not present, then explicitly set {_id: false} to avoid
        // ambiguity in the expected behavior of the serialized projection.
        _root->serialize(explain, &output, options);
        auto idFieldName = options.serializeFieldPath("_id");
        if (output.peek()[idFieldName].missing()) {
            output.addField(idFieldName, Value{false});
        }

        return output.freeze();
    }

    /**
     * Optimize any computed expressions.
     */
    void optimize() final {
        ProjectionExecutor::optimize();
        _root->optimize();
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _root->reportDependencies(deps);
        if (_rootReplacementExpression) {
            expression::addDependencies(_rootReplacementExpression.get(), deps);
        }
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        _root->addVariableRefs(refs);
        if (_rootReplacementExpression) {
            expression::addVariableRefs(_rootReplacementExpression.get(), refs);
        }
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // A root-replacement expression can replace the entire root document, so all paths are
        // considered as modified.
        if (_rootReplacementExpression) {
            return {DocumentSource::GetModPathsReturn::Type::kAllPaths, {}, {}};
        }

        OrderedPathSet preservedPaths;
        _root->reportProjectedPaths(&preservedPaths);

        OrderedPathSet computedPaths;
        StringMap<std::string> renamedPaths;
        _root->reportComputedPaths(&computedPaths, &renamedPaths);

        return {DocumentSource::GetModPathsReturn::Type::kAllExcept,
                std::move(preservedPaths),
                std::move(renamedPaths)};
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
    Document applyProjection(const Document& inputDoc) const final {
        return _root->applyToDocument(inputDoc);
    }

    /**
     * Returns the exhaustive set of all paths that will be preserved by this projection, or
     * boost::none if the exhaustive set cannot be determined.
     */
    boost::optional<std::set<FieldRef>> extractExhaustivePaths() const override {
        std::set<FieldRef> exhaustivePaths;
        DepsTracker depsTracker;
        addDependencies(&depsTracker);
        for (auto&& field : depsTracker.fields) {
            exhaustivePaths.insert(FieldRef{field});
        }

        return exhaustivePaths;
    }

    std::pair<BSONObj, bool> extractComputedProjections(
        const StringData& oldName,
        const StringData& newName,
        const std::set<StringData>& reservedNames) final {
        return _root->extractComputedProjectionsInProject(oldName, newName, reservedNames);
    }

private:
    // The InclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<InclusionNode> _root;
};
}  // namespace mongo::projection_executor
