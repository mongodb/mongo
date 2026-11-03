// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_component_serialization.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
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

BSONObj serializeProjection(const projection_ast::Projection* projection,
                            const SerializationOptions& opts) {
    return projection ? projection_ast::serialize(*projection->root(), opts) : BSONObj{};
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
