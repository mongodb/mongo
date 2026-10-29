// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * GroupFromFirstTransformation consists of a list of (field name, expression pairs). It returns a
 * document synthesized by assigning each field name in the output document to the result of
 * evaluating the corresponding expression. If the expression evaluates to missing, we assign a
 * value of BSONNULL. This is necessary to match the semantics of $first for missing fields.
 */
class GroupFromFirstDocumentTransformation final : public TransformerInterface {
public:
    GroupFromFirstDocumentTransformation(
        const std::string& groupId,
        std::string_view originalStageName,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs,
        AccumulatorDocumentsNeeded docsNeeded = AccumulatorDocumentsNeeded::kFirstInputDocument)
        : _accumulatorExprs(std::move(accumulatorExprs)),
          _groupId(groupId),
          _originalStageName(originalStageName),
          _docsNeeded(docsNeeded) {}

    TransformerType getType() const final {
        return TransformerType::kGroupFromFirstDocument;
    }

    /**
     * The path of the field that we are grouping on: i.e., the field in the input document that we
     * will use to create the _id field of the ouptut document.
     */
    const std::string& groupId() const {
        return _groupId;
    }

    std::string_view originalStageName() const {
        return _originalStageName;
    }

    AccumulatorDocumentsNeeded docsNeeded() const {
        return _docsNeeded;
    }

    Document applyTransformation(const Document& input, const EvaluationContext& ctx) const final;

    void optimize() final;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    Document serializeTransformation(
        const query_shape::SerializationOptions& options = {}) const final;

    DepsTracker::State addDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    void describeTransformation(
        document_transformation::DocumentOperationVisitor& visitor) const final;

    static std::unique_ptr<GroupFromFirstDocumentTransformation> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& groupId,
        std::string_view originalStageName,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs,
        AccumulatorDocumentsNeeded docsNeeded);

private:
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> _accumulatorExprs;
    std::string _groupId;
    std::string_view _originalStageName;
    AccumulatorDocumentsNeeded _docsNeeded;
};

}  // namespace mongo
