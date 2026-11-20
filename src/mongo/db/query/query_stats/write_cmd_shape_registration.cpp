// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/write_cmd_shape_registration.h"

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_shape/bulk_write_cmd_shape.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/server_options.h"
#include "mongo/util/functional.h"

namespace mongo::query_stats {

const FCVGatedFeatureFlag* const UpdateTypes::featureFlag =
    &feature_flags::gFeatureFlagQueryStatsUpdateCommand;
const FCVGatedFeatureFlag* const DeleteTypes::featureFlag =
    &feature_flags::gFeatureFlagQueryStatsDelete;

namespace {

/**
 * Helper with shared checks for when query shape computation should be skipped
 */
template <WriteCommandRequest Request>
bool shouldComputeQueryShape(OperationContext* opCtx,
                             const Request& wholeOp,
                             const FCVGatedFeatureFlag* featureFlag) {
    // Skip computing the shape when the feature flag is disabled.
    if (featureFlag &&
        !featureFlag->isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return false;
    }

    // Skip computing the shape with encrypted fields as indicated by the inclusion of
    // encryptionInformation. It is important to do this before canonicalizing and optimizing
    // the query, each of which would alter the query shape.
    if (wholeOp.getEncryptionInformation()) {
        return false;
    }

    return true;
}

void registerBulkWriteOp(
    OperationContext* opCtx,
    const BulkWriteCommandRequest& request,
    size_t opIdx,
    const NamespaceString& nss,
    query_shape::CollectionType collType,
    function_ref<StatusWith<std::unique_ptr<query_shape::Shape>>()> makeShape) {
    if (!shouldComputeBulkWriteQueryShape(opCtx, request, opIdx, collType)) {
        return;
    }

    query_stats::registerWriteRequest(opCtx, nss, [&]() {
        auto shape = makeShape();
        uassertStatusOKWithContext(shape.getStatus(), "Failed to compute query shape");
        return std::make_unique<query_stats::BulkWriteKey>(
            opCtx, request, opIdx, std::move(shape.getValue()), collType);
    });
}

}  // namespace

void registerBulkWriteUpdateOpQueryStats(OperationContext* opCtx,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const BulkWriteCommandRequest& request,
                                         size_t opIdx,
                                         const ParsedUpdate& parsedUpdate,
                                         query_shape::CollectionType collType) {
    registerBulkWriteOp(
        opCtx, request, opIdx, parsedUpdate.getRequest()->getNamespaceString(), collType, [&]() {
            return shape_helpers::tryMakeShape<query_shape::BulkWriteUpdateCmdShape>(
                request, parsedUpdate, expCtx);
        });
}

void registerBulkWriteDeleteOpQueryStats(OperationContext* opCtx,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const BulkWriteCommandRequest& request,
                                         size_t opIdx,
                                         const ParsedDelete& parsedDelete,
                                         query_shape::CollectionType collType) {
    registerBulkWriteOp(
        opCtx, request, opIdx, parsedDelete.getRequest()->getNsString(), collType, [&]() {
            return shape_helpers::tryMakeShape<query_shape::BulkWriteDeleteCmdShape>(
                request, parsedDelete, expCtx);
        });
}

void registerBulkWriteInsertGroupQueryStats(OperationContext* opCtx,
                                            const BulkWriteCommandRequest& request,
                                            size_t firstOpIdx,
                                            const NamespaceString& nss,
                                            query_shape::CollectionType collType) {
    tassert(13830600,
            "bulkWrite insert group op index is out of range",
            firstOpIdx < request.getOps().size());
    const auto* firstOp = std::get_if<BulkWriteInsertOp>(&request.getOps()[firstOpIdx]);
    tassert(13830601, "bulkWrite insert group must start at an insert op", firstOp);
    tassert(13830602,
            "bulkWrite insert group namespace must match its first op's namespace",
            request.getNsInfo()[firstOp->getNsInfoIdx()].getNs() == nss);

    registerBulkWriteOp(opCtx, request, firstOpIdx, nss, collType, [&]() {
        return shape_helpers::tryMakeShape<query_shape::BulkWriteInsertCmdShape>(request, nss);
    });
}

void computeInsertShapeAndRegisterQueryStats(OperationContext* opCtx,
                                             const write_ops::InsertCommandRequest& wholeOp,
                                             query_shape::CollectionType collType) {

    if (!shouldComputeQueryShape(opCtx, wholeOp, &feature_flags::gFeatureFlagQueryStatsInsert)) {
        return;
    }

    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::InsertCmdShape>(wholeOp,
                                                                        wholeOp.getNamespace());
    }};

    std::ignore = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() -> boost::optional<query_shape::QueryShapeHash> {
            return shape_helpers::computeQueryShapeHash(
                opCtx, deferredShape, wholeOp.getNamespace(), /*skipInternalClientCheck=*/true);
        });

    // Register query stats collection.
    query_stats::registerWriteRequest(opCtx, wholeOp.getNamespace(), [&]() {
        uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
        return std::make_unique<query_stats::InsertKey>(
            opCtx, wholeOp, std::move(deferredShape->getValue()), collType);
    });
}

template <WriteCmdTypes T>
boost::optional<query_shape::DeferredQueryShape> computeAndStoreQueryShapeHash(
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const typename T::Request& wholeOp,
    const typename T::ParsedRequest& parsedRequest) {

    if (!shouldComputeQueryShape(opCtx, wholeOp, T::featureFlag)) {
        return boost::none;
    }

    if constexpr (std::is_same_v<T, UpdateTypes>) {
        if (!isQueryShapeSupportedUpdateType(
                parsedRequest.getRequest()->getUpdateModification().type())) {
            return boost::none;
        }
    }

    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<typename T::CmdShape>(wholeOp, parsedRequest, expCtx);
    }};

    std::ignore = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() -> boost::optional<query_shape::QueryShapeHash> {
            // TODO(SERVER-102484): Provide fast path QueryShape and QueryShapeHash computation
            // for Express queries.
            if (!parsedRequest.hasParsedFindCommand()) {
                return boost::none;
            }
            return shape_helpers::computeQueryShapeHash(
                expCtx, deferredShape, /*skipInternalClientCheck=*/true);
        });

    return deferredShape;
}

template <WriteCmdTypes T>
void computeShapeAndRegisterQueryStats(OperationContext* opCtx,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const typename T::Request& wholeOp,
                                       const typename T::ParsedRequest& parsedRequest,
                                       query_shape::CollectionType collType) {
    boost::optional<query_shape::DeferredQueryShape> maybeDeferredShape =
        computeAndStoreQueryShapeHash<T>(opCtx, expCtx, wholeOp, parsedRequest);
    if (!maybeDeferredShape) {
        return;
    }
    const auto& deferredShape = maybeDeferredShape.get();

    // Register query stats collection.
    query_stats::registerWriteRequest(opCtx, wholeOp.getNamespace(), [&]() {
        uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
        return std::make_unique<typename T::Key>(expCtx,
                                                 wholeOp,
                                                 parsedRequest.getRequest()->getHint(),
                                                 std::move(deferredShape->getValue()),
                                                 collType);
    });
}

// Explicit template instantiations
template boost::optional<query_shape::DeferredQueryShape>
computeAndStoreQueryShapeHash<UpdateTypes>(OperationContext*,
                                           const boost::intrusive_ptr<ExpressionContext>&,
                                           const UpdateTypes::Request&,
                                           const UpdateTypes::ParsedRequest&);

template void computeShapeAndRegisterQueryStats<UpdateTypes>(
    OperationContext*,
    const boost::intrusive_ptr<ExpressionContext>&,
    const UpdateTypes::Request&,
    const UpdateTypes::ParsedRequest&,
    query_shape::CollectionType);

template boost::optional<query_shape::DeferredQueryShape>
computeAndStoreQueryShapeHash<DeleteTypes>(OperationContext*,
                                           const boost::intrusive_ptr<ExpressionContext>&,
                                           const DeleteTypes::Request&,
                                           const DeleteTypes::ParsedRequest&);

template void computeShapeAndRegisterQueryStats<DeleteTypes>(
    OperationContext*,
    const boost::intrusive_ptr<ExpressionContext>&,
    const DeleteTypes::Request&,
    const DeleteTypes::ParsedRequest&,
    query_shape::CollectionType);

}  // namespace mongo::query_stats
