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
