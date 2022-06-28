/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/fcv_op_observer.h"

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
MONGO_FAIL_POINT_DEFINE(pauseBeforeCloseCxns);
MONGO_FAIL_POINT_DEFINE(finishedDropConnections);

void FcvOpObserver::_setVersion(OperationContext* opCtx,
                                multiversion::FeatureCompatibilityVersion newVersion,
                                boost::optional<Timestamp> commitTs) {
    // We set the last FCV update timestamp before setting the new FCV, to make sure we never
    // read an FCV that is not stable.  We might still read a stale one.
    if (commitTs)
        FeatureCompatibilityVersion::advanceLastFCVUpdateTimestamp(*commitTs);
    boost::optional<multiversion::FeatureCompatibilityVersion> prevVersion;

    if (serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        prevVersion = serverGlobalParams.featureCompatibility.getVersion();
    }
    serverGlobalParams.mutableFeatureCompatibility.setVersion(newVersion);
    serverGlobalParams.featureCompatibility.logFCVWithContext("setFCV"_sd);
    FeatureCompatibilityVersion::updateMinWireVersion();

    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            multiversion::GenericFCV::kLatest) ||
        serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
        // minWireVersion == maxWireVersion on kLatest FCV or upgrading/downgrading FCV.
        // Close all incoming connections from internal clients with binary versions lower than
        // ours.
        opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
            transport::Session::kLatestVersionInternalClientKeepOpen |
            transport::Session::kExternalClientKeepOpen);
        // Close all outgoing connections to servers with binary versions lower than ours.
        pauseBeforeCloseCxns.pauseWhileSet();

        executor::EgressTagCloserManager::get(opCtx->getServiceContext())
            .dropConnections(transport::Session::kKeepOpen | transport::Session::kPending);

        if (MONGO_unlikely(finishedDropConnections.shouldFail())) {
            LOGV2(575210, "Hit finishedDropConnections failpoint");
        }
    }

    // We make assumptions that transactions don't span an FCV change. And FCV changes also take
    // the global lock in S mode to create a barrier for operations in IX/X mode, we abort all open
    // transactions here to release the global IX locks held by the transactions more proactively
    // rather than waiting for the transactions to complete. FCV changes take the global S lock when
    // in the upgrading/downgrading state.
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsAbortUnpreparedTransactions(opCtx, matcherAllSessions);
    }

    const auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoordinator->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    // We only want to increment the server TopologyVersion when the minWireVersion has changed.
    // This can only happen in two scenarios:
    // 1. Setting featureCompatibilityVersion from downgrading to fullyDowngraded.
    // 2. Setting featureCompatibilityVersion from fullyDowngraded to upgrading.
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    const auto shouldIncrementTopologyVersion = newVersion == multiversion::GenericFCV::kLastLTS ||
        (prevVersion &&
         prevVersion.get() == multiversion::GenericFCV::kDowngradingFromLatestToLastContinuous) ||
        newVersion == multiversion::GenericFCV::kUpgradingFromLastLTSToLatest ||
        newVersion == multiversion::GenericFCV::kUpgradingFromLastContinuousToLatest ||
        newVersion == multiversion::GenericFCV::kUpgradingFromLastLTSToLastContinuous;

    if (isReplSet && shouldIncrementTopologyVersion) {
        replCoordinator->incrementTopologyVersion();
    }
}

void FcvOpObserver::_onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != multiversion::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersionParser::parse(doc));

    // To avoid extra log messages when the targetVersion is set/unset, only log when the version
    // changes.
    logv2::DynamicAttributes attrs;
    bool isDifferent = true;
    if (serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        const auto currentVersion = serverGlobalParams.featureCompatibility.getVersion();
        attrs.add("currentVersion", multiversion::toString(currentVersion));
        isDifferent = currentVersion != newVersion;
    }

    if (isDifferent) {
        attrs.add("newVersion", multiversion::toString(newVersion));
        LOGV2(20459, "Setting featureCompatibilityVersion", attrs);
    }

    opCtx->recoveryUnit()->onCommit(
        [opCtx, newVersion](boost::optional<Timestamp> ts) { _setVersion(opCtx, newVersion, ts); });
}

void FcvOpObserver::onInserts(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const UUID& uuid,
                              std::vector<InsertStatement>::const_iterator first,
                              std::vector<InsertStatement>::const_iterator last,
                              bool fromMigrate) {
    if (nss.isServerConfigurationCollection()) {
        for (auto it = first; it != last; it++) {
            _onInsertOrUpdate(opCtx, it->doc);
        }
    }
}

void FcvOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (args.updateArgs->update.isEmpty()) {
        return;
    }
    if (args.nss.isServerConfigurationCollection()) {
        _onInsertOrUpdate(opCtx, args.updateArgs->updatedDoc);
    }
}

void FcvOpObserver::onDelete(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const UUID& uuid,
                             StmtId stmtId,
                             const OplogDeleteEntryArgs& args) {
    // documentKeyDecoration is set in OpObserverImpl::aboutToDelete. So the FcvOpObserver
    // relies on the OpObserverImpl also being in the opObserverRegistry.
    auto optDocKey = repl::documentKeyDecoration(opCtx);
    invariant(optDocKey, nss.ns());
    if (nss.isServerConfigurationCollection()) {
        auto id = optDocKey.get().getId().firstElement();
        if (id.type() == BSONType::String && id.String() == multiversion::kParameterName) {
            uasserted(40670, "removing FeatureCompatibilityVersion document is not allowed");
        }
    }
}

void FcvOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {
    // Ensures the in-memory and on-disk FCV states are consistent after a rollback.
    const auto query = BSON("_id" << multiversion::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (swFcv.isOK()) {
        const auto featureCompatibilityVersion = swFcv.getValue();
        auto swVersion = FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion);
        const auto memoryFcv = serverGlobalParams.featureCompatibility.getVersion();
        if (swVersion.isOK() && (swVersion.getValue() != memoryFcv)) {
            auto diskFcv = swVersion.getValue();
            LOGV2(4675801,
                  "Setting featureCompatibilityVersion as part of rollback",
                  "newVersion"_attr = multiversion::toString(diskFcv),
                  "oldVersion"_attr = multiversion::toString(memoryFcv));
            _setVersion(opCtx, diskFcv);
            // The rollback FCV is already in the stable snapshot.
            FeatureCompatibilityVersion::clearLastFCVUpdateTimestamp();
        }
    }
}

}  // namespace mongo
