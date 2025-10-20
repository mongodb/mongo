/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/util/duration.h"
#include <algorithm>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
#include <ratio>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


using namespace fmt::literals;

namespace mongo {
namespace resharding {

namespace {

MONGO_FAIL_POINT_DEFINE(failQueryingRecipients);
MONGO_FAIL_POINT_DEFINE(hangBeforeQueryingRecipients);

BSONObj makeCommandObj(const NamespaceString& ns) {
    auto command = _shardsvrReshardingOperationTime(ns);
    command.setDbName(DatabaseNameUtil::deserialize(
        ns.tenantId(), DatabaseName::kAdmin.db(omitTenant), SerializationContext::stateDefault()));
    return command.toBSON({});
}

auto makeRequests(const BSONObj& cmdObj, const std::vector<ShardId>& recipientShards) {
    invariant(!recipientShards.empty(), "The list of recipient shards cannot be empty");
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& recipient : recipientShards) {
        requests.emplace_back(recipient, cmdObj);
    }
    return requests;
}

}  // namespace

CoordinatorCommitMonitor::CoordinatorCommitMonitor(
    std::shared_ptr<ReshardingMetrics> metrics,
    NamespaceString ns,
    std::vector<ShardId> recipientShards,
    CoordinatorCommitMonitor::TaskExecutorPtr executor,
    CancellationToken cancelToken,
    int delayBeforeInitialQueryMillis,
    Milliseconds maxDelayBetweenQueries)
    : _metrics{std::move(metrics)},
      _ns(std::move(ns)),
      _recipientShards(std::move(recipientShards)),
      _executor(std::move(executor)),
      _cancelToken(std::move(cancelToken)),
      _delayBeforeInitialQueryMillis(Milliseconds(delayBeforeInitialQueryMillis)),
      _maxDelayBetweenQueries(maxDelayBetweenQueries) {}


SemiFuture<void> CoordinatorCommitMonitor::waitUntilRecipientsAreWithinCommitThreshold() const {
    return _makeFuture(_delayBeforeInitialQueryMillis)
        .onError([](Status status) {
            if (ErrorCodes::isCancellationError(status.code()) ||
                ErrorCodes::isInterruption(status.code())) {
                LOGV2_DEBUG(5392003,
                            kDiagnosticLogLevel,
                            "The resharding commit monitor has been interrupted",
                            "error"_attr = status);
            } else {
                LOGV2_WARNING(5392004,
                              "Stopped the resharding commit monitor due to an error",
                              "error"_attr = status);
            }
            return status;
        })
        .semi();
}

void CoordinatorCommitMonitor::setNetworkExecutorForTest(TaskExecutorPtr networkExecutor) {
    invariant(TestingProctor::instance().isEnabled(),
              "Using a separate executor for networking is a test-only feature");
    _networkExecutor = std::move(networkExecutor);
}

CoordinatorCommitMonitor::RemainingOperationTimes
CoordinatorCommitMonitor::queryRemainingOperationTimeForRecipients() const {
    const auto cmdObj = makeCommandObj(_ns);
    const auto requests = makeRequests(cmdObj, _recipientShards);

    LOGV2_DEBUG(5392001,
                kDiagnosticLogLevel,
                "Querying recipient shards for the remaining operation time",
                logAttrs(_ns));

    auto opCtx = CancelableOperationContext(cc().makeOperationContext(), _cancelToken, _executor);
    auto executor = _networkExecutor ? _networkExecutor : _executor;
    AsyncRequestsSender ars(opCtx.get(),
                            executor,
                            DatabaseName::kAdmin,
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent,
                            nullptr /* resourceYielder */,
                            {} /* designatedHostMap */);

    hangBeforeQueryingRecipients.pauseWhileSet();

    auto minRemainingTime = Milliseconds::max();
    auto maxRemainingTime = Milliseconds(0);
    while (!ars.done()) {
        iassert(ErrorCodes::CallbackCanceled,
                "The resharding commit monitor has been canceled",
                !_cancelToken.isCanceled());

        auto response = ars.next();
        auto errorContext =
            "Failed command: {} on {}"_format(cmdObj.toString(), response.shardId.toString());

        auto shardResponse =
            uassertStatusOKWithContext(std::move(response.swResponse), errorContext);
        auto status = getStatusFromCommandResult(shardResponse.data);
        uassertStatusOKWithContext(status, errorContext);

        auto parsedShardResponse = ShardsvrReshardingOperationTimeResponse::parse(
            IDLParserContext("CoordinatorCommitMonitor"), shardResponse.data);
        auto remainingTime = parsedShardResponse.getRemainingMillis();

        // If any recipient omits the "remainingMillis" field of the response then
        // we cannot conclude that it is safe to begin the critical section.
        // It is possible that the recipient just had a failover and
        // was not able to restore its metrics before it replied to the
        // _shardsvrReshardingOperationTime command.
        if (!remainingTime) {
            maxRemainingTime = Milliseconds::max();
            continue;
        }

        if (resharding::gReshardingRemainingTimeEstimateAccountsForRecipientReplicationLag.load()) {
            // The remaining time estimate should account for the replication lag since
            // transitioning to the "strict-consistency" state (or any state) requires waiting for
            // the write to the recipient state doc to be majority committed. If the replication lag
            // info is not available which is expected in a mixed version cluster, assume that it is
            // zero.
            remainingTime = *remainingTime +
                parsedShardResponse.getMajorityReplicationLagMillis().value_or(Milliseconds(0));
        }

        if (remainingTime.value() < minRemainingTime) {
            minRemainingTime = remainingTime.value();
        }
        if (remainingTime.value() > maxRemainingTime) {
            maxRemainingTime = remainingTime.value();
        }
    }

    failQueryingRecipients.execute([](const BSONObj&) {
        iasserted(Status(ErrorCodes::FailPointEnabled, "Querying resharding recipients failed"));
    });

    LOGV2_DEBUG(5392002,
                kDiagnosticLogLevel,
                "Finished querying recipient shards for the remaining operation time",
                logAttrs(_ns),
                "remainingTime"_attr = maxRemainingTime);

    return {minRemainingTime, maxRemainingTime};
}

ExecutorFuture<void> CoordinatorCommitMonitor::_makeFuture(Milliseconds delayBetweenQueries) const {
    return ExecutorFuture<void>(_executor)
        // Start waiting so that we have a more time to calculate a more realistic remaining time
        // estimate.
        .then([this, anchor = shared_from_this(), delayBetweenQueries] {
            return _executor->sleepFor(delayBetweenQueries, _cancelToken)
                .then([this, anchor = std::move(anchor)] {
                    return queryRemainingOperationTimeForRecipients();
                });
        })
        .onError([this](Status status) {
            if (_cancelToken.isCanceled()) {
                // Do not retry on cancellation errors.
                iasserted(status);
            }

            // Absorbs any exception thrown by the query phase, except for cancellation errors, and
            // retries. The intention is to handle short term issues with querying recipients (e.g.,
            // network hiccups and connection timeouts).
            LOGV2_WARNING(5392006,
                          "Encountered an error while querying recipients, will retry shortly",
                          "error"_attr = status);

            // On error we definitely cannot begin the critical section.  Therefore,
            // return Milliseconds::max for remainingTimes.max (remainingTimes.max is used
            // for determining whether the critical section should begin).
            return RemainingOperationTimes{Milliseconds(-1), Milliseconds::max()};
        })
        .then([this, anchor = shared_from_this()](RemainingOperationTimes remainingTimes) mutable {
            auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());

            // If remainingTimes.max (or remainingTimes.min) is Milliseconds::max, then use -1 so
            // that the scale of the y-axis is still useful when looking at FTDC metrics.
            auto clampIfMax = [](Milliseconds t) {
                return t != Milliseconds::max() ? t : Milliseconds(-1);
            };
            _metrics->setCoordinatorHighEstimateRemainingTimeMillis(clampIfMax(remainingTimes.max));
            _metrics->setCoordinatorLowEstimateRemainingTimeMillis(clampIfMax(remainingTimes.min));

            // Check if all recipient shards are within the commit threshold.
            if (remainingTimes.max <= threshold)
                return ExecutorFuture<void>(_executor);

            // The following ensures that the monitor would never sleep for more than a predefined
            // maximum delay between querying recipient shards. Thus, it can handle very large,
            // and potentially inaccurate estimates of the remaining operation time.
            auto delayBetweenQueries =
                std::min(remainingTimes.max - threshold, _maxDelayBetweenQueries);

            return _makeFuture(delayBetweenQueries);
        });
}

}  // namespace resharding
}  // namespace mongo
