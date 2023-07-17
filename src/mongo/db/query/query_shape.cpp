/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape_gen.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo::query_shape {

BSONObj debugPredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    return predicate->serialize(opts);
}
BSONObj representativePredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    return predicate->serialize(opts);
}

BSONObj debugPredicateShape(const MatchExpression* predicate,
                            std::function<std::string(StringData)> transformIdentifiersCallback) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    opts.transformIdentifiersCallback = transformIdentifiersCallback;
    opts.transformIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj representativePredicateShape(
    const MatchExpression* predicate,
    std::function<std::string(StringData)> transformIdentifiersCallback) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    opts.transformIdentifiersCallback = transformIdentifiersCallback;
    opts.transformIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj extractSortShape(const BSONObj& sortSpec,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const SerializationOptions& opts) {
    if (sortSpec.isEmpty()) {
        return sortSpec;
    }
    auto natural = sortSpec[query_request_helper::kNaturalSortField];

    if (!natural) {
        return SortPattern{sortSpec, expCtx}
            .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
            .toBson();
    }
    // This '$natural' will fail to parse as a valid SortPattern since it is not a valid field
    // path - it is usually considered and converted into a hint. For the query shape, we'll
    // keep it unmodified.
    BSONObjBuilder bob;
    for (auto&& elem : sortSpec) {
        if (elem.isABSONObj()) {
            // We expect this won't work or parse on the main command path, but for shapification we
            // don't really care, just treat it as a literal and don't bother parsing.
            opts.appendLiteral(
                &bob, opts.serializeFieldPathFromString(elem.fieldNameStringData()), elem);
        } else if (elem.fieldNameStringData() == natural.fieldNameStringData()) {
            bob.append(elem);
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldNameStringData()));
        }
    }
    return bob.obj();
}

static std::string hintSpecialField = "$hint";
void addShapeLiterals(BSONObjBuilder* bob,
                      const FindCommandRequest& findCommand,
                      const SerializationOptions& opts) {
    if (auto limit = findCommand.getLimit()) {
        opts.appendLiteral(
            bob, FindCommandRequest::kLimitFieldName, static_cast<long long>(*limit));
    }
    if (auto skip = findCommand.getSkip()) {
        opts.appendLiteral(bob, FindCommandRequest::kSkipFieldName, static_cast<long long>(*skip));
    }
}

static std::vector<
    std::pair<StringData, std::function<const OptionalBool(const FindCommandRequest&)>>>
    boolArgMap = {
        {FindCommandRequest::kSingleBatchFieldName, &FindCommandRequest::getSingleBatch},
        {FindCommandRequest::kAllowDiskUseFieldName, &FindCommandRequest::getAllowDiskUse},
        {FindCommandRequest::kReturnKeyFieldName, &FindCommandRequest::getReturnKey},
        {FindCommandRequest::kShowRecordIdFieldName, &FindCommandRequest::getShowRecordId},
        {FindCommandRequest::kTailableFieldName, &FindCommandRequest::getTailable},
        {FindCommandRequest::kAwaitDataFieldName, &FindCommandRequest::getAwaitData},
        {FindCommandRequest::kMirroredFieldName, &FindCommandRequest::getMirrored},
        {FindCommandRequest::kOplogReplayFieldName, &FindCommandRequest::getOplogReplay},
};
std::vector<std::pair<StringData, std::function<const BSONObj(const FindCommandRequest&)>>>
    objArgMap = {
        {FindCommandRequest::kCollationFieldName, &FindCommandRequest::getCollation},

};

void addRemainingFindCommandFields(BSONObjBuilder* bob,
                                   const FindCommandRequest& findCommand,
                                   const SerializationOptions& opts) {
    for (auto [fieldName, getterFunction] : boolArgMap) {
        auto optBool = getterFunction(findCommand);
        optBool.serializeToBSON(fieldName, bob);
    }
    auto collation = findCommand.getCollation();
    if (!collation.isEmpty()) {
        bob->append(FindCommandRequest::kCollationFieldName, collation);
    }
}

BSONObj extractHintShape(BSONObj obj, const SerializationOptions& opts, bool preserveValue) {
    BSONObjBuilder bob;
    for (BSONElement elem : obj) {
        if (hintSpecialField.compare(elem.fieldName()) == 0) {
            if (elem.type() == BSONType::String) {
                bob.append(hintSpecialField, opts.serializeFieldPathFromString(elem.String()));
            } else if (elem.type() == BSONType::Object) {
                opts.appendLiteral(&bob, hintSpecialField, elem.Obj());
            } else {
                uasserted(ErrorCodes::FailedToParse, "$hint must be a string or an object");
            }
            continue;
        }

        // $natural doesn't need to be redacted.
        if (elem.fieldNameStringData().compare(query_request_helper::kNaturalSortField) == 0) {
            bob.append(elem);
            continue;
        }

        if (preserveValue) {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldName()));
        } else {
            opts.appendLiteral(&bob, opts.serializeFieldPathFromString(elem.fieldName()), elem);
        }
    }
    return bob.obj();
}

/**
 * In a let specification all field names are variable names, and all values are either expressions
 * or constants.
 */
BSONObj extractLetSpecShape(BSONObj letSpec,
                            const SerializationOptions& opts,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    BSONObjBuilder bob;
    for (BSONElement elem : letSpec) {
        auto expr = Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState);
        auto redactedValue = expr->serialize(opts);
        // Note that this will throw on deeply nested let variables.
        redactedValue.addToBsonObj(&bob, opts.serializeFieldPathFromString(elem.fieldName()));
    }
    return bob.obj();
}

void appendCmdNs(BSONObjBuilder& bob,
                 const NamespaceString& nss,
                 const SerializationOptions& opts) {
    BSONObjBuilder nsObj = bob.subobjStart("cmdNs");
    appendNamespaceShape(nsObj, nss, opts);
    nsObj.doneFast();
}

void appendNamespaceShape(BSONObjBuilder& bob,
                          const NamespaceString& nss,
                          const SerializationOptions& opts) {
    if (nss.tenantId()) {
        bob.append("tenantId", opts.serializeIdentifier(nss.tenantId().value().toString()));
    }
    bob.append("db", opts.serializeIdentifier(nss.db()));
    bob.append("coll", opts.serializeIdentifier(nss.coll()));
}

BSONObj extractQueryShape(const BSONObj& cmd,
                          const SerializationOptions& opts,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const boost::optional<TenantId>& tenantId) {
    if (cmd.firstElementFieldName() == FindCommandRequest::kCommandName) {
        auto findCommandRequest = std::make_unique<FindCommandRequest>(FindCommandRequest::parse(
            IDLParserContext("findCommandRequest", false /* apiStrict */, tenantId), cmd));
        auto parsedFindCommand =
            uassertStatusOK(parsed_find_command::parse(expCtx, std::move(findCommandRequest)));
        return extractQueryShape(*parsedFindCommand, SerializationOptions(), expCtx);
    } else if (cmd.firstElementFieldName() == AggregateCommandRequest::kCommandName) {
        auto aggregateCommandRequest = AggregateCommandRequest::parse(
            IDLParserContext("aggregateCommandRequest", false /* apiStrict */, tenantId), cmd);
        auto pipeline = Pipeline::parse(aggregateCommandRequest.getPipeline(), expCtx);
        auto ns = aggregateCommandRequest.getNamespace();
        return extractQueryShape(std::move(aggregateCommandRequest),
                                 std::move(*pipeline),
                                 SerializationOptions(),
                                 expCtx,
                                 ns);
    } else {
        uasserted(7746402, str::stream() << "QueryShape can not be computed for command: " << cmd);
    }
}

BSONObj extractQueryShape(const ParsedFindCommand& findRequest,
                          const SerializationOptions& opts,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& findCmd = *findRequest.findCommandRequest;
    BSONObjBuilder bob;
    // Serialize the namespace as part of the query shape.
    {
        auto ns = findCmd.getNamespaceOrUUID();
        if (ns.isNamespaceString()) {
            appendCmdNs(bob, ns.nss(), opts);
        } else {
            BSONObjBuilder cmdNs = bob.subobjStart("cmdNs");
            cmdNs.append("uuid", opts.serializeIdentifier(ns.uuid().toString()));
            cmdNs.append("db", opts.serializeIdentifier(DatabaseNameUtil::serialize(ns.dbName())));
            cmdNs.doneFast();
        }
    }

    bob.append("command", "find");
    std::unique_ptr<MatchExpression> filterExpr;
    // Filter.
    bob.append(FindCommandRequest::kFilterFieldName, findRequest.filter->serialize(opts));

    // Let Spec.
    if (auto letSpec = findCmd.getLet()) {
        auto redactedObj = extractLetSpecShape(letSpec.get(), opts, expCtx);
        auto ownedObj = redactedObj.getOwned();
        bob.append(FindCommandRequest::kLetFieldName, std::move(ownedObj));
    }

    if (findRequest.proj) {
        bob.append(FindCommandRequest::kProjectionFieldName,
                   projection_ast::serialize(*findRequest.proj->root(), opts));
    }

    // Assume the hint is correct and contains field names. It is possible that this hint
    // doesn't actually represent an index, but we can't detect that here.
    // Hint, max, and min won't serialize if the object is empty.
    if (!findCmd.getHint().isEmpty()) {
        bob.append(FindCommandRequest::kHintFieldName,
                   extractHintShape(findCmd.getHint(), opts, true));
        // Max/Min aren't valid without hint.
        if (!findCmd.getMax().isEmpty()) {
            bob.append(FindCommandRequest::kMaxFieldName,
                       extractHintShape(findCmd.getMax(), opts, false));
        }
        if (!findCmd.getMin().isEmpty()) {
            bob.append(FindCommandRequest::kMinFieldName,
                       extractHintShape(findCmd.getMin(), opts, false));
        }
    }

    // Sort.
    if (findRequest.sort) {
        bob.append(
            FindCommandRequest::kSortFieldName,
            findRequest.sort
                ->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
                .toBson());
    }

    // Fields for literal redaction. Adds limit and skip.
    addShapeLiterals(&bob, findCmd, opts);

    // Add the fields that require no redaction.
    addRemainingFindCommandFields(&bob, findCmd, opts);

    return bob.obj();
}

BSONObj extractQueryShape(const AggregateCommandRequest& aggregateCommand,
                          const Pipeline& pipeline,
                          const SerializationOptions& opts,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const NamespaceString& nss) {
    BSONObjBuilder bob;

    // namespace
    appendCmdNs(bob, nss, opts);
    bob.append("command", "aggregate");

    // pipeline
    {
        BSONArrayBuilder pipelineBab(
            bob.subarrayStart(AggregateCommandRequest::kPipelineFieldName));
        auto serializedPipeline = pipeline.serializeToBson(opts);
        for (const auto& stage : serializedPipeline) {
            pipelineBab.append(stage);
        }
        pipelineBab.doneFast();
    }

    // explain
    if (aggregateCommand.getExplain().has_value()) {
        bob.append(AggregateCommandRequest::kExplainFieldName, true);
    }

    // allowDiskUse
    if (auto param = aggregateCommand.getAllowDiskUse(); param.has_value()) {
        bob.append(AggregateCommandRequest::kAllowDiskUseFieldName, param.value_or(false));
    }

    // collation
    if (auto param = aggregateCommand.getCollation()) {
        bob.append(AggregateCommandRequest::kCollationFieldName, param.get());
    }

    // hint
    if (auto hint = aggregateCommand.getHint()) {
        bob.append(AggregateCommandRequest::kHintFieldName,
                   extractHintShape(hint.get(), opts, true));
    }

    // let
    if (auto letSpec = aggregateCommand.getLet()) {
        auto redactedObj = extractLetSpecShape(letSpec.get(), opts, expCtx);
        auto ownedObj = redactedObj.getOwned();
        bob.append(FindCommandRequest::kLetFieldName, std::move(ownedObj));
    }
    return bob.obj();
}

NamespaceStringOrUUID parseNamespaceShape(BSONElement cmdNsElt) {
    tassert(7632900, "cmdNs must be an object.", cmdNsElt.type() == BSONType::Object);
    auto cmdNs = CommandNamespace::parse(IDLParserContext("cmdNs"), cmdNsElt.embeddedObject());

    boost::optional<TenantId> tenantId = cmdNs.getTenantId().map(TenantId::parseFromString);

    if (cmdNs.getColl().has_value()) {
        tassert(7632903,
                "Exactly one of 'uuid' and 'coll' can be defined.",
                !cmdNs.getUuid().has_value());
        return NamespaceString(cmdNs.getDb(), cmdNs.getColl().value());
    } else {
        tassert(7632904,
                "Exactly one of 'uuid' and 'coll' can be defined.",
                !cmdNs.getColl().has_value());
        UUID uuid = uassertStatusOK(UUID::parse(cmdNs.getUuid().value().toString()));
        return NamespaceStringOrUUID(cmdNs.getDb().toString(), uuid, tenantId);
    }
}

QueryShapeHash hash(const BSONObj& queryShape) {
    return QueryShapeHash::computeHash(reinterpret_cast<const uint8_t*>(queryShape.objdata()),
                                       queryShape.objsize());
}
}  // namespace mongo::query_shape
