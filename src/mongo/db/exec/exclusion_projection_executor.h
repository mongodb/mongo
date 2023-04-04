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
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/fastpath_projection_node.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_node.h"
#include "mongo/db/pipeline/expression_dependencies.h"

namespace mongo::projection_executor {
/**
 * A node used to define the parsed structure of an exclusion projection. Each ExclusionNode
 * represents one 'level' of the parsed specification. The root ExclusionNode represents all top
 * level exclusions, with any child ExclusionNodes representing dotted or nested exclusions.
 */
class ExclusionNode : public ProjectionNode {
public:
    ExclusionNode(ProjectionPolicies policies, std::string pathToNode = "")
        : ProjectionNode(policies, std::move(pathToNode)) {}

    ExclusionNode* addOrGetChild(const std::string& field) {
        return static_cast<ExclusionNode*>(ProjectionNode::addOrGetChild(field));
    }

    void reportDependencies(DepsTracker* deps) const final {
        // We have no dependencies on specific fields, since we only know the fields to be removed.
        // We may have expression dependencies though, as $meta expression can be used with
        // exclusion.
        for (auto&& expressionPair : _expressions) {
            expression::addDependencies(expressionPair.second.get(), deps);
        }

        for (auto&& childPair : _children) {
            childPair.second->reportDependencies(deps);
        }
    }

    /**
     * Removes and returns a BSONObj representing the part of this project that depends only on
     * 'oldName'. Also returns a bool indicating whether this entire project is extracted. In the
     * extracted $project, 'oldName' is renamed to 'newName'. 'oldName' should not be dotted.
     */
    std::pair<BSONObj, bool> extractProjectOnFieldAndRename(const StringData& oldName,
                                                            const StringData& newName);

protected:
    std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const {
        return std::make_unique<ExclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }
    MutableDocument initializeOutputDocument(const Document& inputDoc) const final {
        return MutableDocument{inputDoc};
    }
    Value applyLeafProjectionToValue(const Value& value) const final {
        return Value();
    }
    Value transformSkippedValueForOutput(const Value& value) const final {
        return value;
    }
};

/**
 * A fast-path exclusion projection implementation which applies a BSON-to-BSON transformation
 * rather than constructing an output document using the Document/Value API. For exclusion-only
 * projections (as defined by projection_ast::Projection::isExclusionOnly) it can be much faster
 * than the default ExclusionNode implementation. On a document-by-document basis, if the fast-path
 * projection cannot be applied to the input document, it will fall back to the default
 * implementation.
 */
class FastPathEligibleExclusionNode final
    : public FastPathProjectionNode<FastPathEligibleExclusionNode, ExclusionNode> {
private:
    using Base = FastPathProjectionNode<FastPathEligibleExclusionNode, ExclusionNode>;

public:
    using Base::Base;

    Document applyToDocument(const Document& inputDoc) const final;

protected:
    std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const final {
        return std::make_unique<FastPathEligibleExclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }

private:
    void _applyToProjectedField(const BSONElement& element, BSONObjBuilder* bob) const {
        // No-op -- this element is excluded.
    }
    void _applyToNonProjectedField(const BSONElement& element, BSONObjBuilder* bob) const {
        // This element is not excluded by the projection, so it is added to the output.
        bob->append(element);
    }
    void _applyToNonProjectedField(const BSONElement& element, BSONArrayBuilder* bab) const {
        // This array element is not excluded by the projection, so it is added to the output.
        bab->append(element);
    }
    void _applyToRemainingFields(BSONObjIterator& it, BSONObjBuilder* bob) const {
        // We processed all exclusions, rest of the elements are added to the output.
        while (it.more()) {
            bob->append(it.next());
        }
    }

    friend class FastPathProjectionNode<FastPathEligibleExclusionNode, ExclusionNode>;
};

/**
 * A ExclusionProjectionExecutor represents an execution tree for an exclusion projection.
 *
 * This class is mostly a wrapper around an ExclusionNode tree and defers most execution logic to
 * the underlying tree.
 */
class ExclusionProjectionExecutor : public ProjectionExecutor {
public:
    ExclusionProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                ProjectionPolicies policies,
                                bool allowFastPath = false);

    TransformerType getType() const final {
        return TransformerType::kExclusionProjection;
    }

    const ExclusionNode* getRoot() const {
        return _root.get();
    }

    ExclusionNode* getRoot() {
        return _root.get();
    }

    Document serializeTransformation(boost::optional<ExplainOptions::Verbosity> explain,
                                     SerializationOptions options = {}) const final {
        MutableDocument output;

        // The ExclusionNode tree in '_root' will always have a top-level _id node if _id is to be
        // excluded. If the _id node is not present, then explicitly set {_id: true} to avoid
        // ambiguity in the expected behavior of the serialized projection.
        _root->serialize(explain, &output, options);
        auto idFieldName = options.serializeFieldPath("_id");
        if (output.peek()[idFieldName].missing()) {
            output.addField(idFieldName, Value{true});
        }
        return output.freeze();
    }

    /**
     * Exclude the fields specified.
     */
    Document applyProjection(const Document& inputDoc) const final {
        return _root->applyToDocument(inputDoc);
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _root->reportDependencies(deps);
        if (_rootReplacementExpression) {
            expression::addDependencies(_rootReplacementExpression.get(), deps);
        }
        return DepsTracker::State::SEE_NEXT;
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

        OrderedPathSet modifiedPaths;
        _root->reportProjectedPaths(&modifiedPaths);
        return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
    }

    boost::optional<std::set<FieldRef>> extractExhaustivePaths() const {
        return boost::none;
    }

    std::pair<BSONObj, bool> extractProjectOnFieldAndRename(const StringData& oldName,
                                                            const StringData& newName) final {
        return _root->extractProjectOnFieldAndRename(oldName, newName);
    }

private:
    // The ExclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<ExclusionNode> _root;
};
}  // namespace mongo::projection_executor
