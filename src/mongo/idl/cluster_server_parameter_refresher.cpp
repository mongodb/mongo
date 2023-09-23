/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/idl/cluster_server_parameter_refresher.h"
#include "mongo/idl/cluster_server_parameter_refresher_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

MONGO_FAIL_POINT_DEFINE(skipClusterParameterRefresh);
MONGO_FAIL_POINT_DEFINE(blockAndFailClusterParameterRefresh);
MONGO_FAIL_POINT_DEFINE(blockAndSucceedClusterParameterRefresh);
MONGO_FAIL_POINT_DEFINE(countPromiseWaitersClusterParameterRefresh);

namespace mongo {
namespace {

const auto getClusterServerParameterRefresher =
    ServiceContext::declareDecoration<std::unique_ptr<ClusterServerParameterRefresher>>();

Seconds loadInterval() {
    return Seconds(clusterServerParameterRefreshIntervalSecs.load());
}

std::pair<multiversion::FeatureCompatibilityVersion,
          TenantIdMap<stdx::unordered_map<std::string, BSONObj>>>
getFCVAndClusterParametersFromConfigServer() {
    // Use an alternative client region, because we call refreshParameters both from the internal
    // refresher process and from getClusterParameter.
    // Allow this client to be killable. If interrupted, the exception will be caught and handled in
    // refreshParameters.
    auto altClient = getGlobalServiceContext()->makeClient("clusterParameterRefreshTransaction");

    AlternativeClientRegion clientRegion(altClient);
    auto opCtx = cc().makeOperationContext();
    auto as = AuthorizationSession::get(cc());
    as->grantInternalAuthorization(opCtx.get());

    auto configServers = Grid::get(opCtx.get())->shardRegistry()->getConfigShard();
    // Note that we get the list of tenants outside of the transaction. This should be okay, as if
    // we miss out on some new tenants created between this call and the transaction, we are just
    // getting slightly old data. Importantly, different tenants' cluster parameters don't interact
    // with each other, so we don't need a consistent snapshot of cluster parameters across all
    // tenants, just a consistent snapshot per tenant.
    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx.get(), configServers.get()));

    auto allDocs = std::make_shared<TenantIdMap<stdx::unordered_map<std::string, BSONObj>>>();
    auto fcv = std::make_shared<multiversion::FeatureCompatibilityVersion>();
    auto doFetch = [allDocs, fcv, &tenantIds](const txn_api::TransactionClient& txnClient,
                                              ExecutorPtr txnExec) {
        FindCommandRequest findFCV{NamespaceString::kServerConfigurationNamespace};
        findFCV.setFilter(BSON("_id"
                               << "featureCompatibilityVersion"));
        return txnClient.exhaustiveFind(findFCV)
            .thenRunOn(txnExec)
            .then([fcv, allDocs, &tenantIds, &txnClient, txnExec](
                      const std::vector<BSONObj>& foundDocs) {
                uassert(7410710,
                        "Expected to find FCV in admin.system.version but found nothing!",
                        !foundDocs.empty());
                *fcv = FeatureCompatibilityVersionParser::parseVersion(
                    foundDocs[0]["version"].String());

                // Fetch one tenant, then call doFetchTenants for the rest of the tenants within
                // then() recursively.
                auto doFetchTenants = [](auto it,
                                         const auto& tenantIds,
                                         auto allDocs,
                                         const auto& txnClient,
                                         ExecutorPtr txnExec,
                                         auto& doFetchTenants_ref) mutable {
                    if (it == tenantIds.end()) {
                        return SemiFuture<void>::makeReady();
                    }
                    FindCommandRequest findClusterParametersTenant{
                        NamespaceString::makeClusterParametersNSS(*it)};
                    // We don't specify a filter as we want all documents.
                    return txnClient.exhaustiveFind(findClusterParametersTenant)
                        .thenRunOn(txnExec)
                        .then([&doFetchTenants_ref, &txnClient, &tenantIds, txnExec, it, allDocs](
                                  const std::vector<BSONObj>& foundDocs) {
                            stdx::unordered_map<std::string, BSONObj> docsMap;
                            for (const auto& doc : foundDocs) {
                                auto name = doc["_id"].String();
                                docsMap.insert({std::move(name), doc.getOwned()});
                            }
                            allDocs->insert({*it, std::move(docsMap)});
                            return doFetchTenants_ref(std::next(it),
                                                      tenantIds,
                                                      allDocs,
                                                      txnClient,
                                                      txnExec,
                                                      doFetchTenants_ref);
                        })
                        .semi();
                };
                return doFetchTenants(
                    tenantIds.begin(), tenantIds, allDocs, txnClient, txnExec, doFetchTenants);
            })
            .semi();
    };

    repl::ReadConcernArgs::get(opCtx.get()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

    // We need to commit w/ writeConcern = majority for readConcern = snapshot to work.
    opCtx->setWriteConcern(WriteConcernOptions{WriteConcernOptions::kMajority,
                                               WriteConcernOptions::SyncMode::UNSET,
                                               WriteConcernOptions::kNoTimeout});

    auto executor = Grid::get(opCtx.get())->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(opCtx.get(), executor, nullptr, inlineExecutor);
    txn.run(opCtx.get(), doFetch);
    return {*fcv, *allDocs};
}

}  // namespace

Status clusterServerParameterRefreshIntervalSecsNotify(const int& newValue) {
    LOGV2_DEBUG(6226400,
                5,
                "Set clusterServerParameterRefresher interval seconds",
                "clusterServerParameterRefreshIntervalSecs"_attr = loadInterval());
    if (hasGlobalServiceContext()) {
        auto service = getGlobalServiceContext();
        if (getClusterServerParameterRefresher(service)) {
            getClusterServerParameterRefresher(service)->setPeriod(loadInterval());
        }
    }

    return Status::OK();
}

ClusterServerParameterRefresher* ClusterServerParameterRefresher::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ClusterServerParameterRefresher* ClusterServerParameterRefresher::get(ServiceContext* serviceCtx) {
    return getClusterServerParameterRefresher(serviceCtx).get();
}

void ClusterServerParameterRefresher::setPeriod(Milliseconds period) {
    _job->setPeriod(period);
}

Status ClusterServerParameterRefresher::refreshParameters(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_refreshPromise) {
        // We expect the future to never be ready here, because we complete the promise and then
        // delete it under a lock, meaning new futures taken out on the current promise under a lock
        // are always on active promises. If the future is ready here, the below logic will still
        // work, but this is unexpected.
        auto future = _refreshPromise->getFuture();
        if (MONGO_unlikely(future.isReady())) {
            LOGV2_DEBUG(7782200,
                        3,
                        "Cluster parameter refresh request unexpectedly joining on "
                        "already-fulfilled refresh call");
        }
        countPromiseWaitersClusterParameterRefresh.shouldFail();
        // Wait for the job to finish and return its result with getNoThrow.
        lk.unlock();
        return future.getNoThrow();
    }
    // No active job; make a new promise and run the job ourselves.
    _refreshPromise = std::make_unique<SharedPromise<void>>();
    lk.unlock();
    Status status = [&]() {
        try {
            // Run _refreshParameters unlocked to allow new futures to be gotten from our promise.
            return _refreshParameters(opCtx);
        } catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>& ex) {
            return ex.toStatus();
        }
    }();

    lk.lock();
    // Complete the promise and detach it from the object, allowing a new job to be created the
    // next time refreshParameters is run. Note that the futures of this promise hold references to
    // it which will still be valid after we detach it from the object.
    _refreshPromise->setFrom(status);
    _refreshPromise = nullptr;
    return status;
}

Status ClusterServerParameterRefresher::_refreshParameters(OperationContext* opCtx) {
    if (MONGO_unlikely(blockAndFailClusterParameterRefresh.shouldFail())) {
        blockAndFailClusterParameterRefresh.pauseWhileSet();
        return Status(ErrorCodes::FailPointEnabled, "failClusterParameterRefresh was enabled");
    }

    if (MONGO_unlikely(blockAndSucceedClusterParameterRefresh.shouldFail())) {
        blockAndSucceedClusterParameterRefresh.pauseWhileSet();
        return Status::OK();
    }

    invariant(serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));
    multiversion::FeatureCompatibilityVersion fcv;
    TenantIdMap<stdx::unordered_map<std::string, BSONObj>> clusterParameterDocs;

    try {
        std::tie(fcv, clusterParameterDocs) = getFCVAndClusterParametersFromConfigServer();
    } catch (const DBException& ex) {
        LOGV2_WARNING(
            7410719,
            "Could not refresh cluster server parameters from config servers due to failure in "
            "getFCVAndClusterParametersFromConfigServer. Will retry after refresh interval",
            "ex"_attr = ex.toStatus());
        return ex.toStatus();
    }

    // Set each in-memory cluster parameter that was returned in the response.
    bool isSuccessful = true;
    Status status = Status::OK();
    ServerParameterSet* clusterParameterCache = ServerParameterSet::getClusterParameterSet();
    bool fcvChanged = fcv != _lastFcv;
    if (fcvChanged) {
        LOGV2_DEBUG(7410705,
                    3,
                    "Cluster's FCV was different from last during refresh",
                    "oldFCV"_attr = multiversion::toString(_lastFcv),
                    "newFCV"_attr = multiversion::toString(fcv));
    }
    std::vector<BSONObj> allUpdatedParameters;
    allUpdatedParameters.reserve(clusterParameterDocs.size());

    for (const auto& [tenantId, tenantParamDocs] : clusterParameterDocs) {
        std::vector<BSONObj> updatedParameters;
        updatedParameters.reserve(tenantParamDocs.size());
        for (auto [name, sp] : clusterParameterCache->getMap()) {
            if (fcvChanged) {
                // Use canBeEnabled because if we previously temporarily disabled the parameter,
                // isEnabled will be false
                if (sp->canBeEnabledOnVersion(_lastFcv) && !sp->canBeEnabledOnVersion(fcv)) {
                    // Parameter is newly disabled on cluster
                    LOGV2_DEBUG(
                        7410703, 3, "Disabling parameter during refresh", "name"_attr = name);
                    sp->disable(false /* permanent */);
                    continue;
                } else if (sp->canBeEnabledOnVersion(fcv) && !sp->canBeEnabledOnVersion(_lastFcv)) {
                    // Parameter is newly enabled on cluster
                    LOGV2_DEBUG(
                        7410704, 3, "Enabling parameter during refresh", "name"_attr = name);
                    sp->enable();
                }
            }
            if (!sp->isEnabled()) {
                continue;
            }
            BSONObjBuilder oldClusterParameterBob;
            sp->append(opCtx, &oldClusterParameterBob, name, tenantId);

            auto it = tenantParamDocs.find(name);
            if (it == tenantParamDocs.end()) {
                // Reset the local parameter to its default value.
                status = sp->reset(tenantId);
            } else {
                // Set the local parameter to the pulled value.
                const auto& clusterParameterDoc = it->second;
                status = sp->set(clusterParameterDoc, tenantId);
            }

            if (!status.isOK()) {
                LOGV2_WARNING(6226402,
                              "Could not (re)set in-memory cluster server parameter",
                              "parameter"_attr = name,
                              "tenantId"_attr = tenantId,
                              "presentOnConfigSvr"_attr = it != tenantParamDocs.end(),
                              "reason"_attr = status.reason());
                isSuccessful = false;
            }

            BSONObjBuilder updatedClusterParameterBob;
            sp->append(opCtx, &updatedClusterParameterBob, name, tenantId);
            BSONObj updatedClusterParameterBSON = updatedClusterParameterBob.obj().getOwned();

            audit::logUpdateCachedClusterParameter(opCtx->getClient(),
                                                   oldClusterParameterBob.obj().getOwned(),
                                                   updatedClusterParameterBSON,
                                                   tenantId);
            if (it != tenantParamDocs.end()) {
                updatedParameters.emplace_back(
                    updatedClusterParameterBSON.removeField("clusterParameterTime"_sd));
            }
        }
        auto tenantIdStr = tenantId ? tenantId->toString() : "none";
        allUpdatedParameters.emplace_back(
            BSON("tenantId" << tenantIdStr << "updatedParameters" << updatedParameters));
    }

    if (isSuccessful) {
        LOGV2_DEBUG(6226403,
                    3,
                    "Updated cluster server parameters",
                    "clusterParameterDocuments"_attr = allUpdatedParameters);
    }

    _lastFcv = fcv;

    return status;
}

void ClusterServerParameterRefresher::start(ServiceContext* serviceCtx, OperationContext* opCtx) {
    auto refresher = std::make_unique<ClusterServerParameterRefresher>();
    // On mongos, this should always be true after FCV initialization
    // (Generic FCV reference):
    invariant(serverGlobalParams.featureCompatibility.getVersion() ==
              multiversion::GenericFCV::kLatest);
    refresher->_lastFcv = serverGlobalParams.featureCompatibility.getVersion();
    auto periodicRunner = serviceCtx->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "ClusterServerParameterRefresher",
        [serviceCtx](Client* client) { getClusterServerParameterRefresher(serviceCtx)->run(); },
        loadInterval(),
        // TODO(SERVER-74659): Please revisit if this periodic job could be made killable.
        false /*isKillableByStepdown*/);

    refresher->_job = std::make_unique<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    // Make sure the invalidator is moved to the service context by the time we call start()
    getClusterServerParameterRefresher(serviceCtx) = std::move(refresher);
    getClusterServerParameterRefresher(serviceCtx)->_job->start();
}

void ClusterServerParameterRefresher::run() {
    if (MONGO_unlikely(skipClusterParameterRefresh.shouldFail())) {
        return;
    }

    auto opCtx = cc().makeOperationContext();
    auto status = refreshParameters(opCtx.get());
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6226405, 1, "Cluster server parameter refresh failed", "reason"_attr = status.reason());
    }
}

void ClusterServerParameterRefresher::onShutdown(ServiceContext* serviceCtx) {
    // Make sure that we finish the possibly running transaction and don't start any more.
    auto& refresher = getClusterServerParameterRefresher(serviceCtx);
    if (refresher && refresher->_job && refresher->_job->isValid()) {
        refresher->_job->pause();
    }
}

}  // namespace mongo
