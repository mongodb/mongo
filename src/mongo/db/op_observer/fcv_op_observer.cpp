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


#include "mongo/db/op_observer/fcv_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/executor/egress_connection_closer_manager.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
MONGO_FAIL_POINT_DEFINE(pauseBeforeCloseCxns);
MONGO_FAIL_POINT_DEFINE(finishedDropConnections);

void FcvOpObserver::_setVersion(OperationContext* opCtx,
                                multiversion::FeatureCompatibilityVersion newVersion,
                                bool onRollback,
                                bool withinRecoveryUnit,
                                boost::optional<Timestamp> commitTs) {
    // We set the last FCV update timestamp before setting the new FCV, to make sure we never
    // read an FCV that is not stable.  We might still read a stale one.
    if (commitTs)
        FeatureCompatibilityVersion::advanceLastFCVUpdateTimestamp(*commitTs);
    boost::optional<multiversion::FeatureCompatibilityVersion> prevVersion;

    const auto prevFcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (prevFcvSnapshot.isVersionInitialized()) {
        prevVersion = prevFcvSnapshot.getVersion();
    }
    serverGlobalParams.mutableFCV.setVersion(newVersion);

    const auto newFcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    newFcvSnapshot.logFCVWithContext("setFCV"_sd);
    FeatureCompatibilityVersion::updateMinWireVersion(opCtx);

    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (newFcvSnapshot.isGreaterThanOrEqualTo(multiversion::GenericFCV::kLatest) ||
        newFcvSnapshot.isUpgradingOrDowngrading()) {
        // minWireVersion == maxWireVersion on kLatest FCV or upgrading/downgrading FCV.
        // Close all incoming connections from internal clients with binary versions lower than
        // ours.
        if (auto tlm = opCtx->getServiceContext()->getTransportLayerManager()) {
            tlm->endAllSessions(Client::kLatestVersionInternalClientKeepOpen |
                                Client::kExternalClientKeepOpen);
        }

        // Close all outgoing connections to servers with binary versions lower than ours.
        pauseBeforeCloseCxns.pauseWhileSet();

        executor::EgressConnectionCloserManager::get(opCtx->getServiceContext())
            .dropConnections(Status(ErrorCodes::PooledConnectionsDropped,
                                    "Closing connection to servers with lower binary versions"));

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
    try {
        if (newFcvSnapshot.isUpgradingOrDowngrading()) {
            SessionKiller::Matcher matcherAllSessions(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
            killSessionsAbortUnpreparedTransactions(
                opCtx, matcherAllSessions, ErrorCodes::InterruptedDueToFCVChange);
        }
    } catch (const DBException&) {
        // Swallow the error when running within a recovery unit to avoid process termination.
        // The failure can be ignored here, assuming that the setFCV command will also be
        // interrupted on _prepareToUpgrade/Downgrade() or earlier.
        if (!withinRecoveryUnit) {
            throw;
        }
    }

    const auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet = replCoordinator->getSettings().isReplSet();
    // We only want to increment the server TopologyVersion when the minWireVersion has changed.
    // This can happen in the following cases:
    // 1. Setting featureCompatibilityVersion from downgrading to fullyDowngraded.
    // 2. Setting featureCompatibilityVersion from fullyDowngraded to upgrading.
    // 3. Rollback from downgrading to fullyUpgraded.
    // 4. Rollback from fullyDowngraded to downgrading.
    // Note that the minWireVersion does not change between downgrading -> isCleaningServerMetadata,
    // as the FCV of the isCleaningServerMetadata is still kDowngrading, so the prevVersion and
    // newVersion will be equal
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    const auto shouldIncrementTopologyVersion =
        (newVersion == multiversion::GenericFCV::kLastLTS ||
         (prevVersion &&
          prevVersion.value() ==
              multiversion::GenericFCV::kDowngradingFromLatestToLastContinuous) ||
         newVersion == multiversion::GenericFCV::kUpgradingFromLastLTSToLatest ||
         newVersion == multiversion::GenericFCV::kUpgradingFromLastContinuousToLatest ||
         newVersion == multiversion::GenericFCV::kUpgradingFromLastLTSToLastContinuous ||
         // (Generic FCV reference): This FCV check should exist across LTS binary versions.
         (onRollback &&
          (prevVersion == multiversion::GenericFCV::kLastLTS ||
           newVersion == multiversion::GenericFCV::kLatest))) &&
        !(prevVersion && prevVersion.value() == newVersion);
    if (isReplSet && shouldIncrementTopologyVersion) {
        replCoordinator->incrementTopologyVersion();
    }
}

void FcvOpObserver::_onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::string ||
        idElement.String() != multiversion::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersionParser::parse(doc));

    // To avoid extra log messages when the targetVersion is set/unset, only log when the
    // version changes.
    logv2::DynamicAttributes attrs;
    bool isDifferent = true;
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (fcvSnapshot.isVersionInitialized()) {
        const auto currentVersion = fcvSnapshot.getVersion();
        attrs.add("currentVersion", multiversion::toString(currentVersion));
        isDifferent = currentVersion != newVersion;
    }

    if (isDifferent) {
        attrs.add("newVersion", multiversion::toString(newVersion));
        LOGV2(20459, "Setting featureCompatibilityVersion", attrs);
    }

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [newVersion](OperationContext* opCtx, boost::optional<Timestamp> ts) {
            _setVersion(opCtx, newVersion, false /*onRollback*/, true /*withinRecoveryUnit*/, ts);
        });
}

void FcvOpObserver::onInserts(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              std::vector<InsertStatement>::const_iterator first,
                              std::vector<InsertStatement>::const_iterator last,
                              const std::vector<RecordId>& recordIds,
                              std::vector<bool> fromMigrate,
                              bool defaultFromMigrate,
                              OpStateAccumulator* opAccumulator) {
    if (coll->ns().isServerConfigurationCollection()) {
        for (auto it = first; it != last; it++) {
            _onInsertOrUpdate(opCtx, it->doc);
        }
    }
}

void FcvOpObserver::onUpdate(OperationContext* opCtx,
                             const OplogUpdateEntryArgs& args,
                             OpStateAccumulator* opAccumulator) {
    if (args.updateArgs->update.isEmpty()) {
        return;
    }
    if (args.coll->ns().isServerConfigurationCollection()) {
        _onInsertOrUpdate(opCtx, args.updateArgs->updatedDoc);
    }
}

void FcvOpObserver::onDelete(OperationContext* opCtx,
                             const CollectionPtr& coll,
                             StmtId stmtId,
                             const BSONObj& doc,
                             const DocumentKey& documentKey,
                             const OplogDeleteEntryArgs& args,
                             OpStateAccumulator* opAccumulator) {
    if (coll->ns().isServerConfigurationCollection()) {
        auto id = documentKey.getId().firstElement();
        if (id.type() == BSONType::string && id.String() == multiversion::kParameterName) {
            uasserted(40670, "removing FeatureCompatibilityVersion document is not allowed");
        }
    }
}

void FcvOpObserver::onReplicationRollback(OperationContext* opCtx,
                                          const RollbackObserverInfo& rbInfo) {
    // Ensures the in-memory and on-disk FCV states are consistent after a rollback.
    const auto query = BSON("_id" << multiversion::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (swFcv.isOK()) {
        const auto featureCompatibilityVersion = swFcv.getValue();
        auto swVersion = FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion);
        const auto memoryFcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
        if (swVersion.isOK() && (swVersion.getValue() != memoryFcv)) {
            auto diskFcv = swVersion.getValue();
            LOGV2(4675801,
                  "Setting featureCompatibilityVersion as part of rollback",
                  "newVersion"_attr = multiversion::toString(diskFcv),
                  "oldVersion"_attr = multiversion::toString(memoryFcv));
            _setVersion(opCtx, diskFcv, true /*onRollback*/, false /*withinRecoveryUnit*/);
            // The rollback FCV is already in the stable snapshot.
            FeatureCompatibilityVersion::clearLastFCVUpdateTimestamp();
        }
    }
}

}  // namespace mongo
