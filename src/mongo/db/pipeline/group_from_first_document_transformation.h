/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
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

#include <memory>
#include <set>
#include <string>
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
        StringData originalStageName,
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

    StringData originalStageName() const {
        return _originalStageName;
    }

    AccumulatorDocumentsNeeded docsNeeded() const {
        return _docsNeeded;
    }

    Document applyTransformation(const Document& input) const final;

    void optimize() final;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    Document serializeTransformation(const SerializationOptions& options = {}) const final;

    DepsTracker::State addDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    static std::unique_ptr<GroupFromFirstDocumentTransformation> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& groupId,
        StringData originalStageName,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs,
        AccumulatorDocumentsNeeded docsNeeded);

private:
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> _accumulatorExprs;
    std::string _groupId;
    StringData _originalStageName;
    AccumulatorDocumentsNeeded _docsNeeded;
};

}  // namespace mongo
