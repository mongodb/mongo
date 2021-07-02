
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/status.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;

Lock::ResourceMutex FeatureCompatibilityVersion::fcvLock("featureCompatibilityVersionLock");
// lastFCVUpdateTimestamp contains the latest oplog entry timestamp which updated the FCV.
// It is reset on rollback.
Timestamp lastFCVUpdateTimestamp;
SimpleMutex lastFCVUpdateTimestampMutex;

void FeatureCompatibilityVersion::setTargetUpgrade(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersionParser::kVersionField,
                          FeatureCompatibilityVersionParser::kVersion36);
        updateMods.append(FeatureCompatibilityVersionParser::kTargetVersionField,
                          FeatureCompatibilityVersionParser::kVersion40);
    });
}

void FeatureCompatibilityVersion::setTargetDowngrade(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersionParser::kVersionField,
                          FeatureCompatibilityVersionParser::kVersion36);
        updateMods.append(FeatureCompatibilityVersionParser::kTargetVersionField,
                          FeatureCompatibilityVersionParser::kVersion36);
    });
}

void FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(OperationContext* opCtx,
                                                                StringData version) {
    _validateVersion(version);

    // Updates 'version' field, while also unsetting the 'targetVersion' field.
    _runUpdateCommand(opCtx, [version](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersionParser::kVersionField, version);
    });
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* opCtx,
                                                    repl::StorageInterface* storageInterface) {
    if (!isCleanStartUp())
        return;

    // If the server was not started with --shardsvr, the default featureCompatibilityVersion on
    // clean startup is the upgrade version. If it was started with --shardsvr, the default
    // featureCompatibilityVersion is the downgrade version, so that it can be safely added to a
    // downgrade version cluster. The config server will run setFeatureCompatibilityVersion as part
    // of addShard.
    const bool storeUpgradeVersion = serverGlobalParams.clusterRole != ClusterRole::ShardServer;

    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
    NamespaceString nss(NamespaceString::kServerConfigurationNamespace);

    {
        CollectionOptions options;
        options.uuid = CollectionUUID::gen();
        uassertStatusOK(storageInterface->createCollection(opCtx, nss, options));
    }

    // We then insert the featureCompatibilityVersion document into the server configuration
    // collection. The server parameter will be updated on commit by the op observer.
    uassertStatusOK(storageInterface->insertDocument(
        opCtx,
        nss,
        repl::TimestampedBSONObj{
            BSON("_id" << FeatureCompatibilityVersionParser::kParameterName
                       << FeatureCompatibilityVersionParser::kVersionField
                       << (storeUpgradeVersion ? FeatureCompatibilityVersionParser::kVersion40
                                               : FeatureCompatibilityVersionParser::kVersion36)),
            Timestamp()},
        repl::OpTime::kUninitializedTerm));  // No timestamp or term because this write is not
                                             // replicated.
}

bool FeatureCompatibilityVersion::isCleanStartUp() {
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    storageEngine->listDatabases(&dbNames);

    for (auto&& dbName : dbNames) {
        if (dbName != "local") {
            return false;
        }
    }
    return true;
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersionParser::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersionParser::parse(doc));

    // To avoid extra log messages when the targetVersion is set/unset, only log when the version
    // changes.
    bool isDifferent = serverGlobalParams.featureCompatibility.isVersionInitialized()
        ? serverGlobalParams.featureCompatibility.getVersion() != newVersion
        : true;
    if (isDifferent) {
        log() << "setting featureCompatibilityVersion to "
              << FeatureCompatibilityVersionParser::toString(newVersion);
    }

    opCtx->recoveryUnit()->onCommit(
        [opCtx, newVersion](boost::optional<Timestamp> ts) { _setVersion(opCtx, newVersion, ts); });
}

void FeatureCompatibilityVersion::updateMinWireVersion() {
    WireSpec& spec = WireSpec::instance();

    switch (serverGlobalParams.featureCompatibility.getVersion()) {
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40:
        case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION - 1;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION - 1;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior:
            // getVersion() does not return this value.
            MONGO_UNREACHABLE;
    }
}

void FeatureCompatibilityVersion::_setVersion(
    OperationContext* opCtx,
    ServerGlobalParams::FeatureCompatibility::Version newVersion,
    boost::optional<Timestamp> commitTs) {
    // We set the last FCV update timestamp before setting the new FCV, to make sure we never
    // read an FCV that is not stable.  We might still read a stale one.
    {
        stdx::lock_guard<SimpleMutex> lk(lastFCVUpdateTimestampMutex);
        if (commitTs && *commitTs > lastFCVUpdateTimestamp) {
            lastFCVUpdateTimestamp = *commitTs;
        }
    }
    serverGlobalParams.featureCompatibility.setVersion(newVersion);
    updateMinWireVersion();

    if (ShardingState::get(opCtx)->enabled() &&
        (newVersion == ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36 ||
         newVersion == ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40)) {
        Grid::get(opCtx)->catalogCache()->purgeAllDatabases();
    }

    if (newVersion != ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36) {
        // Close all incoming connections from internal clients with binary versions lower than
        // ours.
        opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
            transport::Session::kLatestVersionInternalClientKeepOpen |
            transport::Session::kExternalClientKeepOpen);
        // Close all outgoing connections to servers with binary versions lower than ours.
        executor::EgressTagCloserManager::get(opCtx->getServiceContext())
            .dropConnections(transport::Session::kKeepOpen);
    }

    if (newVersion != ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
        // Transactions are only allowed when the featureCompatibilityVersion is 4.0, so abort
        // any open transactions when downgrading featureCompatibilityVersion.
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsLocalKillTransactions(opCtx, matcherAllSessions);
    }
}

void FeatureCompatibilityVersion::onReplicationRollback(OperationContext* opCtx) {
    const auto query = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (swFcv.isOK()) {
        const auto featureCompatibilityVersion = swFcv.getValue();
        auto swVersion = FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion);
        const auto memoryFcv = serverGlobalParams.featureCompatibility.getVersion();
        if (swVersion.isOK() && (swVersion.getValue() != memoryFcv)) {
            auto diskFcv = swVersion.getValue();
            log() << "Setting featureCompatibilityVersion from '"
                  << FeatureCompatibilityVersionParser::toString(memoryFcv) << "' to '"
                  << FeatureCompatibilityVersionParser::toString(diskFcv)
                  << "' as part of rollback.";
            _setVersion(opCtx, diskFcv, boost::none);
            // The rollback FCV is already in the stable snapshot.
            stdx::lock_guard<SimpleMutex> lk(lastFCVUpdateTimestampMutex);
            lastFCVUpdateTimestamp = Timestamp();
        }
    }
}

void FeatureCompatibilityVersion::_validateVersion(StringData version) {
    uassert(40284,
            str::stream() << "featureCompatibilityVersion must be '"
                          << FeatureCompatibilityVersionParser::kVersion40
                          << "' or '"
                          << FeatureCompatibilityVersionParser::kVersion36
                          << "'. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".",
            version == FeatureCompatibilityVersionParser::kVersion40 ||
                version == FeatureCompatibilityVersionParser::kVersion36);
}

void FeatureCompatibilityVersion::_runUpdateCommand(OperationContext* opCtx,
                                                    UpdateBuilder builder) {
    DBDirectClient client(opCtx);
    NamespaceString nss(NamespaceString::kServerConfigurationNamespace);

    BSONObjBuilder updateCmd;
    updateCmd.append("update", nss.coll());
    {
        BSONArrayBuilder updates(updateCmd.subarrayStart("updates"));
        {
            BSONObjBuilder updateSpec(updates.subobjStart());
            {
                BSONObjBuilder queryFilter(updateSpec.subobjStart("q"));
                queryFilter.append("_id", FeatureCompatibilityVersionParser::kParameterName);
            }
            {
                BSONObjBuilder updateMods(updateSpec.subobjStart("u"));
                builder(std::move(updateMods));
            }
            updateSpec.appendBool("upsert", true);
        }
    }
    updateCmd.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Majority);

    // Update the featureCompatibilityVersion document stored in the server configuration
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.db().toString(), updateCmd.obj(), updateResult);
    uassertStatusOK(getStatusFromWriteCommandReply(updateResult));
}

/**
 * Read-only server parameter for featureCompatibilityVersion.
 */
class FeatureCompatibilityVersionParameter : public ServerParameter {
public:
    FeatureCompatibilityVersionParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          FeatureCompatibilityVersionParser::kParameterName.toString(),
                          false,  // allowedToChangeAtStartup
                          false   // allowedToChangeAtRuntime
                          ) {}

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        BSONObjBuilder featureCompatibilityVersionBuilder(b.subobjStart(name));
        uassert(ErrorCodes::UnknownFeatureCompatibilityVersion,
                str::stream() << FeatureCompatibilityVersionParser::kParameterName
                              << " is not yet known.",
                serverGlobalParams.featureCompatibility.isVersionInitialized());
        switch (serverGlobalParams.featureCompatibility.getVersion()) {
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kVersionField,
                    FeatureCompatibilityVersionParser::kVersion40);
                break;
            case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kVersionField,
                    FeatureCompatibilityVersionParser::kVersion36);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kTargetVersionField,
                    FeatureCompatibilityVersionParser::kVersion40);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kVersionField,
                    FeatureCompatibilityVersionParser::kVersion36);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kTargetVersionField,
                    FeatureCompatibilityVersionParser::kVersion36);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersionParser::kVersionField,
                    FeatureCompatibilityVersionParser::kVersion36);
                break;
            case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior:
                // getVersion() does not return this value.
                MONGO_UNREACHABLE;
        }
        // If the FCV has been recently set to the fully upgraded FCV but is not part of the
        // majority snapshot, then if we do a binary upgrade, we may see the old FCV at startup.
        // It is not safe to do oplog application on the new binary at that point.  So we make sure
        // that when we report the FCV, it is in the majority snapshot.
        // (The same consideration applies at downgrade, where if a recently-set fully downgraded
        // FCV is not part of the majority snapshot, the downgraded binary will see the upgrade FCV
        // and fail.)
        const auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet = replCoordinator &&
            replCoordinator->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
        auto neededMajorityTimestamp = [] {
            stdx::lock_guard<SimpleMutex> lk(lastFCVUpdateTimestampMutex);
            return lastFCVUpdateTimestamp;
        }();
        if (isReplSet && !neededMajorityTimestamp.isNull()) {
            auto status = replCoordinator->waitUntilOpTimeForRead(
                opCtx,
                repl::ReadConcernArgs(
                    repl::OpTime(neededMajorityTimestamp, repl::OpTime::kUninitializedTerm),
                    repl::ReadConcernLevel::kMajorityReadConcern));
            // If majority reads are not supported, we will take a full snapshot on clean shutdown
            // and the new FCV will be included, so upgrade is possible.
            if (status.code() != ErrorCodes::ReadConcernMajorityNotEnabled)
                uassertStatusOK(
                    status.withContext("Most recent 'featureCompatibilityVersion' was not in the "
                                       "majority snapshot on this node"));
        }
        return;
    }

    virtual Status set(const BSONElement& newValueElement) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream()
                          << FeatureCompatibilityVersionParser::kParameterName
                          << " cannot be set via setParameter. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
    }

    virtual Status setFromString(const std::string& str) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream()
                          << FeatureCompatibilityVersionParser::kParameterName
                          << " cannot be set via setParameter. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
    }
} featureCompatibilityVersionParameter;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(internalValidateFeaturesAsMaster, bool, true);

}  // namespace mongo
