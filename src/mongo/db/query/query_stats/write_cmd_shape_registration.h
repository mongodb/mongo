// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"

#include <concepts>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

template <typename Request>
concept WriteCommandRequest = requires(const Request& r) {
    { r.getWriteCommandRequestBase() } -> std::same_as<const write_ops::WriteCommandRequestBase&>;
};

template <typename T>
concept WriteCmdTypes = requires {
    typename T::CmdShape;
    typename T::Key;
    typename T::Request;
    typename T::ParsedRequest;
    { T::featureFlag } -> std::convertible_to<const FCVGatedFeatureFlag*>;
    requires WriteCommandRequest<typename T::Request>;
};

struct UpdateTypes {
    using CmdShape = query_shape::UpdateCmdShape;
    using Key = query_stats::UpdateKey;
    using Request = write_ops::UpdateCommandRequest;
    using ParsedRequest = ParsedUpdate;
    static const FCVGatedFeatureFlag* const featureFlag;
};

struct DeleteTypes {
    using CmdShape = query_shape::DeleteCmdShape;
    using Key = query_stats::DeleteKey;
    using Request = write_ops::DeleteCommandRequest;
    using ParsedRequest = ParsedDelete;
    static const FCVGatedFeatureFlag* const featureFlag;
};

/**
 * Computes the query shape of an update or delete command and records its hash in CurOp (for slow
 * query logs)
 *
 * Returns a DeferredQueryShape for the command, or boost::none if query stats collection is
 * unsupported for this command (ex feature flag disabled, or encrypted fields present).
 */
template <WriteCmdTypes T>
boost::optional<query_shape::DeferredQueryShape> computeAndStoreQueryShapeHash(
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const typename T::Request& wholeOp,
    const typename T::ParsedRequest& parsedRequest);

/**
 * Computes the query shape of an update or delete command, records its hash in CurOp (for slow
 * query logs), and registers it with the query stats store.
 *
 * Skips computing the shape if query stats collection is unsupported for this command (ex feature
 * flag disabled, or encrypted fields present).
 */
template <WriteCmdTypes T>
void computeShapeAndRegisterQueryStats(OperationContext* opCtx,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const typename T::Request& wholeOp,
                                       const typename T::ParsedRequest& parsedRequest,
                                       query_shape::CollectionType collType);

/**
 * Computes the query shape of an insert command, records its hash in CurOp (for slow query logs),
 * and registers it with the query stats store.
 *
 * Skips computing the shape if query stats collection is unsupported for this command (ex feature
 * flag disabled, or encrypted fields present).
 */
void computeInsertShapeAndRegisterQueryStats(OperationContext* opCtx,
                                             const write_ops::InsertCommandRequest& wholeOp,
                                             query_shape::CollectionType collType);

}  // namespace mongo::query_stats
