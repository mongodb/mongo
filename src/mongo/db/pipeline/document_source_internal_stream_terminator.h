// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * StageParams for $_internalStreamTerminator and takes no arguments.
 */
class InternalStreamTerminatorStageParams : public DefaultStageParams {
public:
    InternalStreamTerminatorStageParams(BSONElement element) : DefaultStageParams(element) {}
    static const Id& id;
    Id getId() const final {
        return id;
    }
};

class InternalStreamTerminatorLiteParsed final
    : public LiteParsedDocumentSourceDefault<InternalStreamTerminatorLiteParsed> {
public:
    InternalStreamTerminatorLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<InternalStreamTerminatorLiteParsed>(originalBson) {}

    static std::unique_ptr<InternalStreamTerminatorLiteParsed> parse(
        const NamespaceString& nss,
        const BSONElement& originalBson,
        const LiteParserOptions& options) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "$_internalStreamTerminator must take a nested object but found: "
                              << originalBson,
                originalBson.type() == BSONType::object);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$_internalStreamTerminator must take an empty object but found: "
                              << originalBson.embeddedObject(),
                originalBson.embeddedObject().isEmpty());
        return std::make_unique<InternalStreamTerminatorLiteParsed>(originalBson);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalStreamTerminatorStageParams>(_originalBson);
    }
};

/**
 * Returns EOF and disposes source upon seeing an {_eos: true} sentinel. Prevents Exchange
 * deadlock when the doc consumer's buffer fills before the meta consumer sees natural EOF.
 */
class DocumentSourceInternalStreamTerminator final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalStreamTerminator"sv;
    static constexpr std::string_view kEosFieldName = "_eos"sv;

    explicit DocumentSourceInternalStreamTerminator(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kNotAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kNotAllowed,
                                UnionRequirement::kNotAllowed);
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;
};

}  // namespace mongo
