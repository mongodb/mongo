/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalAssertDataAssumptions);
class InternalAssertDataAssumptionsLiteParsed final
    : public LiteParsedDocumentSourceDefault<InternalAssertDataAssumptionsLiteParsed> {
public:
    InternalAssertDataAssumptionsLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<InternalAssertDataAssumptionsLiteParsed>(originalBson) {}

    static std::unique_ptr<InternalAssertDataAssumptionsLiteParsed> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
        return std::make_unique<InternalAssertDataAssumptionsLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalAssertDataAssumptionsStageParams>(_originalBson);
    }

    bool isSelectionStage() const final {
        return true;
    }
};

/**
 * $_internalAssertDataAssumptions is a test-only internal stage that validates the dependency
 * graph's arrayness analysis. It accepts a set of FieldPaths and for each input document, asserts
 * that none of those fields contain arrays. This stage is used in conjunction with the
 * internalEnableDependencyGraphValidation query knob and is automatically inserted by a pipeline
 * rewrite pass before stages where the dependency graph reports canPathBeArray() == false for
 * certain fields.
 *
 * The purpose of this stage is to catch bugs in the dependency graph's arrayness tracking by
 * validating at runtime that fields claimed to be non-array actually do not contain arrays.
 *
 * This is a passthrough stage that does not modify documents, it only validates them.
 */
class DocumentSourceInternalAssertDataAssumptions final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalAssertDataAssumptions"_sd;

    /**
     * Creates a DocumentSourceInternalAssertDataAssumptions from a BSONElement specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a DocumentSourceInternalAssertDataAssumptions with the given set of field paths
     * that must not contain arrays.
     */
    static boost::intrusive_ptr<DocumentSourceInternalAssertDataAssumptions> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths);

    DocumentSourceInternalAssertDataAssumptions(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths);

    StringData getSourceName() const final {
        return kStageName;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    static const Id& id;

    Id getId() const final {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.canRunOnTimeseries = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    const std::set<FieldPath>& getNonArrayPaths() const {
        return _nonArrayPaths;
    }

private:
    // Set of field paths that must not contain arrays in any document
    std::set<FieldPath> _nonArrayPaths;
};

}  // namespace mongo
