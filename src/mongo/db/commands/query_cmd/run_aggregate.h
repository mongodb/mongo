// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/ifr_flag_retry_info.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/util/retry.h"
#include "mongo/db/shard_role/shard_catalog/external_data_source_scope_guard.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Executes the aggregation 'request' over the specified namespace 'nss' using context 'opCtx'.
 *
 * The raw aggregate command parameters should be passed in 'cmdObj', and will be reported as the
 * originatingCommand in subsequent getMores on the resulting agg cursor.
 *
 * 'privileges' contains the privileges that were required to run this aggregation, to be used later
 * for re-checking privileges for getMore commands.
 *
 * On success, fills out 'result' with the command response.
 */
Status runAggregate(
    OperationContext* opCtx,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    const BSONObj& cmdObj,
    const PrivilegeVector& privileges,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    rpc::ReplyBuilderInterface* result,
    const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
        usedExternalDataSources = {});

/**
 * Returns true if an 'ErrorCodes::IFRFlagRetry' kickback raised while running 'request' must be
 * propagated to the router that produced the request instead of being absorbed and retried locally.
 */
bool shouldPropagateIfrKickbackToRouter(OperationContext* opCtx,
                                        const AggregateCommandRequest& request);

/**
 * Runs 'fn', which executes 'request' as an aggregation that the calling command derived locally,
 * retrying it with the offending feature flag disabled if it raises 'ErrorCodes::IFRFlagRetry'.
 */
template <typename Fn>
auto retryOnLocalIFRFlagKickback(OperationContext* opCtx,
                                 const AggregateCommandRequest& request,
                                 std::string_view opName,
                                 Fn&& fn) {
    return retryOn<ErrorCodes::IFRFlagRetry>(
        opName,
        std::forward<Fn>(fn),
        kDefaultMaxRetries,
        [&](const ExceptionFor<ErrorCodes::IFRFlagRetry>& ex) {
            // If the error wasn't propagated, then 'runAggregate' has locally attempted and failed
            // to retry this request, so we should throw this error.
            if (!shouldPropagateIfrKickbackToRouter(opCtx, request)) {
                throw ex;
            }

            auto retryInfo = ex.extraInfo<IFRFlagRetryInfo>();
            tassert(13248903, "IFR retry is missing its IFRFlagRetryInfo", retryInfo);

            std::string_view disabledFlagName = retryInfo->getDisabledFlagName();
            auto* flag = IncrementalRolloutFeatureFlag::findByName(disabledFlagName);
            tassert(13248904,
                    str::stream() << "IFR retry referenced an unknown feature flag: "
                                  << disabledFlagName,
                    flag);

            IncrementalFeatureRolloutContext::get(opCtx)->disableFlag(*flag);
        });
}

/**
 * Tracks explicit use of allowDiskUse:false with find and aggregate commands.
 */
extern Counter64& allowDiskUseFalseCounter;
}  // namespace mongo
