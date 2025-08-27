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


#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <ratio>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace resharding {

namespace {

MONGO_FAIL_POINT_DEFINE(failQueryingRecipients);
MONGO_FAIL_POINT_DEFINE(hangBeforeQueryingRecipients);

BSONObj makeCommandObj(const NamespaceString& ns) {
    auto command = _shardsvrReshardingOperationTime(ns);
    command.setDbName(DatabaseNameUtil::deserialize(
        ns.tenantId(), DatabaseName::kAdmin.db(omitTenant), SerializationContext::stateDefault()));
    return command.toBSON();
}

auto makeRequests(const BSONObj& cmdObj, const std::set<ShardId>& participantShards) {
    invariant(!participantShards.empty(), "The list of participant shards cannot be empty");
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& participantShard : participantShards) {
        requests.emplace_back(participantShard, cmdObj);
    }
    return requests;
}

Milliseconds add(const Milliseconds& lhs, const Milliseconds& rhs) {
    if (lhs == Milliseconds::max() || rhs == Milliseconds::max()) {
        return Milliseconds::max();
    }
    return lhs + rhs;
}

}  // namespace

CoordinatorCommitMonitor::CoordinatorCommitMonitor(
    std::shared_ptr<ReshardingMetrics> metrics,
    NamespaceString ns,
    std::vector<ShardId> donorShards,
    std::vector<ShardId> recipientShards,
    CoordinatorCommitMonitor::TaskExecutorPtr executor,
    CancellationToken cancelToken,
    int delayBeforeInitialQueryMillis)
    : _metrics{std::move(metrics)},
      _ns(std::move(ns)),
      _donorShards(donorShards.begin(), donorShards.end()),
      _recipientShards(recipientShards.begin(), recipientShards.end()),
      _participantShards([&] {
          std::set<ShardId> participantShards(donorShards.begin(), donorShards.end());
          participantShards.insert(_recipientShards.begin(), _recipientShards.end());
          return participantShards;
      }()),
      _executor(std::move(executor)),
      _cancelToken(std::move(cancelToken)),
      _delayBeforeInitialQueryMillis(Milliseconds(delayBeforeInitialQueryMillis)) {
    invariant(!_donorShards.empty(), "The list of donor shards cannot be empty");
    invariant(!_recipientShards.empty(), "The list of recipient shards cannot be empty");
}


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
CoordinatorCommitMonitor::queryRemainingOperationTime() const {
    const auto cmdObj = makeCommandObj(_ns);
    const auto requests = makeRequests(cmdObj, _participantShards);

    hangBeforeQueryingRecipients.pauseWhileSet();

    LOGV2_DEBUG(5392001,
                kDiagnosticLogLevel,
                "Querying participant shards to estimate the remaining operation time",
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


    auto minTimeToEnterCriticalSection = Milliseconds::max();
    auto maxTimeToEnterCriticalSection = Milliseconds(0);

    auto minTimeToExitCriticalSection = Milliseconds::max();
    auto maxTimeToExitCriticalSection = Milliseconds(0);

    // Unless explicitly opted out, the remaining time estimate should account for the replication
    // lag on each donor since transition to the "blocking-writes" state (or any state) involves:
    // - Performing a shard version refresh which involves doing a noop write and waiting for the
    //   write to be majority committed.
    // - Processing the refreshed resharding fields which involves writing the configTime to the
    //   config.vectorClock collection and waiting for the write to be majority committed.
    bool accountForDonorReplLag =
        resharding::gReshardingRemainingTimeEstimateAccountsForDonorReplicationLag.load();
    // Unless explicitly opted out, the remaining time estimate should account for the replication
    // lag on each recipient since transitioning to the "strict-consistency" state (or any state)
    // involves waiting for the write to the recipient state doc to be majority committed.
    bool accountForRecipientReplLag =
        resharding::gReshardingRemainingTimeEstimateAccountsForRecipientReplicationLag.load();

    while (!ars.done()) {
        iassert(ErrorCodes::CallbackCanceled,
                "The resharding commit monitor has been canceled",
                !_cancelToken.isCanceled());

        auto response = ars.next();
        auto errorContext =
            fmt::format("Failed command: {} on {}", cmdObj.toString(), response.shardId.toString());

        auto shardResponse =
            uassertStatusOKWithContext(std::move(response.swResponse), errorContext);
        auto status = getStatusFromCommandResult(shardResponse.data);
        uassertStatusOKWithContext(status, errorContext);

        auto parsedShardResponse = ShardsvrReshardingOperationTimeResponse::parse(
            shardResponse.data, IDLParserContext("CoordinatorCommitMonitor"));
        // If the replication lag info is not available which is expected in a mixed version
        // cluster, assume that it is zero.
        auto replicationLag =
            parsedShardResponse.getMajorityReplicationLagMillis().value_or(Milliseconds(0));

        if (_donorShards.contains(response.shardId)) {
            auto timeToEnterCriticalSection =
                accountForDonorReplLag ? replicationLag : Milliseconds(0);
            if (timeToEnterCriticalSection < minTimeToEnterCriticalSection) {
                minTimeToEnterCriticalSection = timeToEnterCriticalSection;
            }
            if (timeToEnterCriticalSection > maxTimeToEnterCriticalSection) {
                maxTimeToEnterCriticalSection = timeToEnterCriticalSection;
            }
        }

        if (_recipientShards.contains(response.shardId)) {
            // If any recipient omits the remaining time field of the response then we cannot
            // conclude that it is safe to begin the critical section. It is possible that the
            // recipient just had a failover and was not able to restore its metrics before it
            // replied to the _shardsvrReshardingOperationTime command.
            auto timeToExitCriticalSection =
                add(parsedShardResponse.getRecipientRemainingMillis().value_or(Milliseconds::max()),
                    accountForRecipientReplLag ? replicationLag : Milliseconds(0));

            if (timeToExitCriticalSection < minTimeToExitCriticalSection) {
                minTimeToExitCriticalSection = timeToExitCriticalSection;
            }
            if (timeToExitCriticalSection > maxTimeToExitCriticalSection) {
                maxTimeToExitCriticalSection = timeToExitCriticalSection;
            }
        }
    }

    failQueryingRecipients.execute([](const BSONObj&) {
        iasserted(Status(ErrorCodes::FailPointEnabled, "Querying resharding recipients failed"));
    });

    auto minRemainingOperationTime =
        add(minTimeToEnterCriticalSection, minTimeToExitCriticalSection);
    auto maxRemainingOperationTime =
        add(maxTimeToEnterCriticalSection, maxTimeToExitCriticalSection);

    LOGV2_DEBUG(10430301,
                kDiagnosticLogLevel,
                "Finished querying participant shards to estimate the time to enter and exit the "
                "critical section",
                logAttrs(_ns),
                "minTimeToEnter"_attr = minTimeToEnterCriticalSection,
                "maxTimeToEnter"_attr = maxTimeToEnterCriticalSection,
                "minTimeToExit"_attr = minTimeToExitCriticalSection,
                "maxTimeToExit"_attr = maxTimeToExitCriticalSection);

    LOGV2_DEBUG(5392002,
                kDiagnosticLogLevel,
                "Finished querying participant shards to estimate the remaining operation time",
                logAttrs(_ns),
                "remainingTime"_attr = maxRemainingOperationTime);

    return {minRemainingOperationTime, maxRemainingOperationTime};
}

ExecutorFuture<void> CoordinatorCommitMonitor::_makeFuture(Milliseconds delayBetweenQueries) const {
    auto delay = std::make_shared<Milliseconds>(delayBetweenQueries);
    // We do not use withDelayBetweenIterations option because we want to delay the initial query.
    return AsyncTry([this, anchor = shared_from_this(), delay] {
               return _executor->sleepFor(*delay, _cancelToken)
                   .then([this, anchor = std::move(anchor)] {
                       return queryRemainingOperationTime();
                   });
           })
        .until([this, anchor = shared_from_this(), delay](
                   const StatusWith<RemainingOperationTimes> result) -> bool {
            auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
            auto maxDelayBetweenQueries =
                Milliseconds(gReshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis.load());

            RemainingOperationTimes remainingTimes;
            if (!result.isOK()) {
                if (_cancelToken.isCanceled()) {
                    // Do not retry on cancellation errors.
                    iasserted(result.getStatus());
                }
                // Absorbs any exception thrown by the query phase, except for cancellation errors,
                // and retries. The intention is to handle short term issues with querying
                // recipients (e.g., network hiccups and connection timeouts).
                LOGV2_WARNING(5392006,
                              "Encountered an error while querying recipients, will retry shortly",
                              "error"_attr = result.getStatus());
                // On error we definitely cannot begin the critical section.  Therefore,
                // return Milliseconds::max for remainingTimes.max (remainingTimes.max is used
                // for determining whether the critical section should begin).
                remainingTimes = RemainingOperationTimes{Milliseconds(-1), Milliseconds::max()};
            } else {
                remainingTimes = result.getValue();
            }

            // If remainingTimes.max (or remainingTimes.min) is Milliseconds::max, then use -1 so
            // that the scale of the y-axis is still useful when looking at FTDC metrics.
            auto clampIfMax = [](Milliseconds t) {
                return t != Milliseconds::max() ? t : Milliseconds(-1);
            };
            _metrics->setCoordinatorHighEstimateRemainingTimeMillis(clampIfMax(remainingTimes.max));
            _metrics->setCoordinatorLowEstimateRemainingTimeMillis(clampIfMax(remainingTimes.min));

            // Check if all recipient shards are within the commit threshold.
            if (remainingTimes.max <= threshold)
                return true;

            // The following ensures that the monitor would never sleep for more than a predefined
            // maximum delay between querying recipient shards. Thus, it can handle very large,
            // and potentially inaccurate estimates of the remaining operation time.
            *delay = std::min(remainingTimes.max - threshold, maxDelayBetweenQueries);

            return false;
        })
        .on(_executor, _cancelToken)
        .onCompletion([](StatusWith<CoordinatorCommitMonitor::RemainingOperationTimes> statusWith) {
            return mongo::Future<void>::makeReady(statusWith.getStatus());
        });
}

}  // namespace resharding
}  // namespace mongo
