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

#include "mongo/platform/basic.h"

#include "mongo/idl/cluster_server_parameter_refresher.h"

#include "mongo/db/audit.h"
#include "mongo/db/commands/list_databases_for_all_tenants_gen.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/idl/cluster_server_parameter_refresher_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

MONGO_FAIL_POINT_DEFINE(skipClusterParameterRefresh);
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
    auto altClient = getGlobalServiceContext()->makeClient("clusterParameterRefreshTransaction");
    // TODO(SERVER-74660): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(*altClient.get());
        altClient.get()->setSystemOperationUnkillableByStepdown(lk);
    }
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
        FindCommandRequest findFCV{NamespaceString("admin.system.version")};
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
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);
    txn_api::SyncTransactionWithRetries txn(
        opCtx.get(), sleepInlineExecutor, nullptr, inlineExecutor);
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
    invariant(isMongos());
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
        refresher->_job->stop();
    }
    getClusterServerParameterRefresher(serviceCtx) = nullptr;
}

}  // namespace mongo
