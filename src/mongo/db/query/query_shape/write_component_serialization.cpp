// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_component_serialization.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/update/pipeline_executor.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_tree_executor.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo::query_shape {
namespace {

using namespace std::literals::string_view_literals;

BSONObj serializeWriteQueryImpl(const ParsedFindCommand* parsedFind,
                                const BSONObj& rawQuery,
                                const SerializationOptions& opts) {
    if (parsedFind) {
        auto matchExpr = parsedFind->filter.get();
        return matchExpr ? matchExpr->serialize(opts) : BSONObj{};
    }

    // The only requests without a ParsedFindCommand are simple _id equality queries.
    BSONElement idElem = rawQuery["_id"];
    tassert(13742605, "Expected simple _id query", !idElem.eoo());
    dassert(isSimpleIdQuery(rawQuery));

    BSONElement valueElem = idElem;
    if (idElem.type() == BSONType::object &&
        idElem.Obj().firstElementFieldNameStringData() == "$eq"sv) {
        valueElem = idElem.Obj().firstElement();
    }

    BSONObjBuilder result;
    BSONObjBuilder idObj(result.subobjStart(opts.serializeFieldPath("_id")));
    opts.appendLiteral(&idObj, "$eq", valueElem);
    idObj.doneFast();
    return result.obj();
}

/**
 * Reconstructs a no-op modifier update. Serializing an all-no-op modifier normally produces {},
 * which would be interpreted as a replacement update if the representative shape is reparsed.
 */
BSONObj makeNoopModifierUpdateOpShape(const BSONObj& updateModifier) {
    BSONObjIteratorSorted iter(updateModifier);
    BSONObjBuilder bob;
    while (iter.more()) {
        auto element = iter.next();
        tassert(11560300,
                "A no-op modifier update is expected to have an empty object for each operator.",
                element.isABSONObj() && element.Obj().isEmpty());
        bob.append(element);
    }
    return bob.obj();
}

}  // namespace

BSONObj serializeWriteQuery(const ParsedUpdate& parsedUpdate, const SerializationOptions& opts) {
    return serializeWriteQueryImpl(
        parsedUpdate.parsedFind.get(), parsedUpdate.getRequest()->getQuery(), opts);
}

BSONObj serializeWriteQuery(const ParsedDelete& parsedDelete, const SerializationOptions& opts) {
    return serializeWriteQueryImpl(
        parsedDelete.parsedFind.get(), parsedDelete.getRequest()->getQuery(), opts);
}

BSONObj serializeFindAndModifySort(const ParsedFindCommand* parsedFind,
                                   const BSONObj& rawSort,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const SerializationOptions& opts) {
    if (parsedFind && parsedFind->sort) {
        return parsedFind->sort
            ->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
            .toBson();
    }

    if (rawSort.isEmpty() || rawSort[query_request_helper::kNaturalSortField]) {
        return BSONObj{};
    }

    std::unique_ptr<SortPattern> sortPattern;
    try {
        sortPattern = std::make_unique<SortPattern>(rawSort, expCtx);
    } catch (const DBException&) {
        // The simple _id fast path does not parse or use the sort. Do not let query-shape
        // serialization reject a command that the execution path accepts.
        return BSONObj{};
    }
    return sortPattern
        ->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
        .toBson();
}

BSONObj serializeProjection(const projection_ast::Projection* projection,
                            const SerializationOptions& opts) {
    return projection ? projection_ast::serialize(*projection->root(), opts) : BSONObj{};
}

projection_ast::Projection parseFindAndModifyProjection(
    const ParsedFindCommand* parsedFind,
    const BSONObj& rawQuery,
    const BSONObj& rawProjection,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback) {
    std::unique_ptr<MatchExpression> parsedFilter;
    const MatchExpression* filter = parsedFind ? parsedFind->filter.get() : nullptr;
    if (!filter) {
        // Parsed writes skip parsing simple _id queries. A returned-document projection can still
        // require the matcher (for example, positional projection), so parse the original query
        // locally without canonicalizing or mutating the execution request.
        auto parsed = MatchExpressionParser::parse(
            rawQuery, expCtx, extensionsCallback, MatchExpressionParser::kAllowAllSpecialFeatures);
        parsedFilter = uassertStatusOK(std::move(parsed));
        filter = parsedFilter.get();
    }

    return projection_ast::parseAndAnalyze(
        expCtx, rawProjection, filter, rawQuery, ProjectionPolicies::findProjectionPolicies());
}

BSONObj serializeFindAndModifyProjection(const ParsedFindCommand* parsedFind,
                                         const BSONObj& rawQuery,
                                         const BSONObj& rawProjection,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const ExtensionsCallback& extensionsCallback,
                                         const SerializationOptions& opts) {
    // An absent or empty 'fields' contributes nothing to the shape. Return before parsing so that
    // the simple _id fast path does not parse the matcher solely for an empty projection.
    if (rawProjection.isEmpty()) {
        return BSONObj{};
    }

    auto projection = parseFindAndModifyProjection(
        parsedFind, rawQuery, rawProjection, expCtx, extensionsCallback);

    // Projection parsing can reference system variables that the parsed query did not (for
    // example $$USER_ROLES in 'fields' only), so those are not yet initialized. Initialize them so
    // serialization emits the variable's constant value rather than a reference.
    expCtx->initializeReferencedSystemVariables();

    return serializeProjection(&projection, opts);
}

Value serializeUpdate(const ParsedUpdate& parsedUpdate, const SerializationOptions& opts) {
    const auto modType = parsedUpdate.getRequest()->getUpdateModification().type();
    const auto* executor = parsedUpdate.getDriver()->getUpdateExecutor();
    switch (modType) {
        case write_ops::UpdateModification::Type::kReplacement:
            return opts.serializeLiteral(
                parsedUpdate.getRequest()->getUpdateModification().getUpdateReplacement());
        case write_ops::UpdateModification::Type::kPipeline:
            return Value(static_cast<const PipelineExecutor*>(executor)->serialize(opts));
        case write_ops::UpdateModification::Type::kModifier: {
            SerializationOptions modifierOpts = opts;
            if (!(parsedUpdate.arrayFilters == nullptr || parsedUpdate.arrayFilters->empty())) {
                modifierOpts.serializeForUpdateArrayFilters = true;
            }

            auto value = static_cast<const UpdateTreeExecutor*>(executor)->serialize(modifierOpts);
            return value.getDocument().empty()
                ? Value(makeNoopModifierUpdateOpShape(
                      parsedUpdate.getRequest()->getUpdateModification().getUpdateModifier()))
                : value;
        }
        default:
            MONGO_UNREACHABLE_TASSERT(11034402);
    }
    return {};
}

boost::optional<BSONObj> serializeUpdateConstants(const ParsedUpdate& parsedUpdate,
                                                  const SerializationOptions& opts) {
    if (parsedUpdate.getDriver()->type() != UpdateDriver::UpdateType::kPipeline) {
        return boost::none;
    }

    const boost::optional<BSONObj>& constants = parsedUpdate.getRequest()->getUpdateConstants();
    if (!constants || constants->isEmpty()) {
        return boost::none;
    }

    BSONObjBuilder shapifiedConstants;
    for (const auto& elem : *constants) {
        const auto varName = elem.fieldNameStringData();
        opts.serializeLiteral(elem).addToBsonObj(&shapifiedConstants,
                                                 opts.serializeFieldPathFromString(varName));
    }
    return shapifiedConstants.obj();
}

boost::optional<std::vector<BSONObj>> serializeArrayFilters(const ParsedUpdate& parsedUpdate,
                                                            const SerializationOptions& opts) {
    if (parsedUpdate.getDriver()->type() != UpdateDriver::UpdateType::kOperator ||
        parsedUpdate.arrayFilters == nullptr || parsedUpdate.arrayFilters->empty()) {
        return boost::none;
    }

    std::vector<BSONObj> serializedFilters;
    for (const auto& [identifier, expression] : *parsedUpdate.arrayFilters) {
        serializedFilters.push_back(expression->getFilter()->serialize(opts));
    }
    return serializedFilters;
}

}  // namespace mongo::query_shape
