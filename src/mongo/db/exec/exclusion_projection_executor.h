// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/fastpath_projection_node.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_node.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    std::pair<BSONObj, bool> extractProjectOnFieldAndRename(std::string_view oldName,
                                                            std::string_view newName);

protected:
    Type getType() const override {
        return Type::kExclusion;
    }

    std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const override {
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
    bool isIncluded() const final {
        return false;
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

    Document applyToDocument(const Document& inputDoc, const EvaluationContext& ctx) const final;

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

    Document serializeTransformation(
        const query_shape::SerializationOptions& options = {}) const final {
        MutableDocument output;

        // The ExclusionNode tree in '_root' will always have a top-level _id node if _id is to be
        // excluded. If the _id node is not present, then explicitly set {_id: true} to avoid
        // ambiguity in the expected behavior of the serialized projection.
        _root->serialize(&output, options);
        auto idFieldName = options.serializeFieldPath("_id");
        if (output.peek()[std::string_view{idFieldName}].missing()) {
            output.addField(idFieldName, Value{true});
        }
        return output.freeze();
    }

    /**
     * Exclude the fields specified.
     */
    Document applyProjection(const Document& inputDoc, const EvaluationContext& ctx) const final {
        return _root->applyToDocument(inputDoc, ctx);
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

        OrderedPathSet computedPaths;
        StringMap<std::string> renamedPaths;
        StringMap<std::string> complexRenamedPaths;
        _root->reportComputedPaths(&computedPaths, &renamedPaths, &complexRenamedPaths);

        if (computedPaths.empty()) {
            return {
                DocumentSource::GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
        } else {
            // The only case where computedPaths would be non-empty is if there is a $meta
            // expression in the exclude projection. This could result in dependencies for
            // subsequent stages--e.g., a $match on the $meta field, in which case the $match should
            // NOT be pushed in front of the $project. If $meta is not identified as a dependency,
            // $match WOULD be pushed in front of $project during pipeline optimization.
            //
            // $meta is the only expression that is permitted in an exclusion pipeline, so, rather
            // than establishing a dependency analysis workflow for exclusion projections just for
            // this case, we simply return a kNotSupported type GetModPathsReturn so that pipeline
            // optimization does not occur.
            //
            // TODO SERVER-100587 Fix dependency analysis to enable pipeline optimization when $meta
            // is used in an exclusion projection
            return {DocumentSource::GetModPathsReturn::Type::kNotSupported, {}, {}};
        }
    }

    void describeTransformation(
        document_transformation::DocumentOperationVisitor& visitor) const override;

    boost::optional<std::set<FieldRef>> extractExhaustivePaths() const override {
        return boost::none;
    }

    std::pair<BSONObj, bool> extractProjectOnFieldAndRename(std::string_view oldName,
                                                            std::string_view newName) final {
        return _root->extractProjectOnFieldAndRename(oldName, newName);
    }

private:
    // The ExclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<ExclusionNode> _root;
};
}  // namespace mongo::projection_executor
