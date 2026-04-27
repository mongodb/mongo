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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace MONGO_MOD_NEEDS_REPLACEMENT mongo {

/**
 * Parameters produced by LiteParsedGraphLookUp::getStageParams() and consumed by
 * DocumentSourceGraphLookUp::createFromStageParams(). Carries the lite-parsed values so the
 * full parse step only needs to handle what requires an ExpressionContext (the startWith
 * Expression and MatchExpression validation for restrictSearchWithMatch).
 *
 * The 'startWith' BSONElement aliases the originalBson held by the enclosing
 * LiteParsedGraphLookUp. It is only valid as long as that LiteParsed is alive.
 */
class GraphLookUpStageParams : public DefaultStageParams {
public:
    GraphLookUpStageParams(NamespaceString from,
                           boost::optional<FieldPath> as,
                           boost::optional<FieldPath> connectFromField,
                           boost::optional<FieldPath> connectToField,
                           boost::optional<BSONElement> startWith,
                           boost::optional<BSONObj> additionalFilter,
                           boost::optional<FieldPath> depthField,
                           boost::optional<long long> maxDepth,
                           BSONElement originalBson)
        : DefaultStageParams(originalBson),
          from(std::move(from)),
          as(std::move(as)),
          connectFromField(std::move(connectFromField)),
          connectToField(std::move(connectToField)),
          startWith(startWith),
          additionalFilter(std::move(additionalFilter)),
          depthField(std::move(depthField)),
          maxDepth(maxDepth) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    NamespaceString from;
    // 'as', 'connectFromField', 'connectToField', and 'startWith' are required by $graphLookup,
    // but the lite-parse stage only needs 'from' to compute privileges. Required-field
    // presence is enforced by DocumentSourceGraphLookUp::createFromStageParams.
    boost::optional<FieldPath> as;
    boost::optional<FieldPath> connectFromField;
    boost::optional<FieldPath> connectToField;
    boost::optional<BSONElement> startWith;
    boost::optional<BSONObj> additionalFilter;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
};

/**
 * Lite-parse representation of $graphLookup
 */
class LiteParsedGraphLookUp final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedGraphLookUp> {
public:
    static constexpr StringData kStageName = "$graphLookup"_sd;

    static std::unique_ptr<LiteParsedGraphLookUp> parse(const NamespaceString& nss,
                                                        const BSONElement& spec,
                                                        const LiteParserOptions& options);

    LiteParsedGraphLookUp(const BSONElement& spec,
                          NamespaceString foreignNss,
                          boost::optional<FieldPath> as,
                          boost::optional<FieldPath> connectFromField,
                          boost::optional<FieldPath> connectToField,
                          boost::optional<BSONElement> startWith,
                          boost::optional<BSONObj> additionalFilter,
                          boost::optional<FieldPath> depthField,
                          boost::optional<long long> maxDepth);

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final;

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const override;

    std::unique_ptr<StageParams> getStageParams() const override;

private:
    boost::optional<FieldPath> _as;
    boost::optional<FieldPath> _connectFromField;
    boost::optional<FieldPath> _connectToField;
    boost::optional<BSONElement> _startWith;
    boost::optional<BSONObj> _additionalFilter;
    boost::optional<FieldPath> _depthField;
    boost::optional<long long> _maxDepth;
};

}  // namespace MONGO_MOD_NEEDS_REPLACEMENT mongo
