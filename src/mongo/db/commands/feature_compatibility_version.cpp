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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version.h"

#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;
using FCVP = FeatureCompatibilityVersionParser;
using FCVParams = ServerGlobalParams::FeatureCompatibility;
using FCV = FCVParams::Version;

using namespace fmt::literals;

Lock::ResourceMutex FeatureCompatibilityVersion::fcvLock("featureCompatibilityVersionLock");

namespace {

/**
 * Utility class for recording permitted transitions between feature compatibility versions and
 * their on-disk representation as FeatureCompatibilityVersionDocument objects.
 */
const class FCVTransitions {
public:
    FCVTransitions() {
        auto makeFCVDoc = [](
                              // What features we support during the transition.
                              FCV effectiveVersion,
                              // The transition's goal FCV.
                              boost::optional<FCV> targetVersion = boost::none,
                              // The previous FCV while downgrading.
                              boost::optional<FCV> previousVersion = boost::none) {
            FeatureCompatibilityVersionDocument fcvDoc;
            fcvDoc.setVersion(effectiveVersion);
            fcvDoc.setTargetVersion(targetVersion);
            fcvDoc.setPreviousVersion(previousVersion);
            return fcvDoc;
        };

        // Steady states - they have FCV documents but no _transitions entries.
        for (auto fcv :
             std::vector{FCVParams::kLastLTS, FCVParams::kLastContinuous, FCVParams::kLatest}) {
            _fcvDocuments[fcv] = makeFCVDoc(fcv);
        }

        for (auto [from, upgrading, to] :
             std::vector{std::make_tuple(FCVParams::kLastLTS,
                                         FCVParams::kUpgradingFromLastLTSToLatest,
                                         FCVParams::kLatest),
                         std::make_tuple(FCVParams::kLastContinuous,
                                         FCVParams::kUpgradingFromLastContinuousToLatest,
                                         FCVParams::kLatest)}) {
            for (auto isFromConfigServer : std::vector{false, true}) {
                // Start or complete upgrading to latest. If this release's lastContinuous ==
                // lastLTS then the second loop iteration just overwrites the first.
                _transitions[{from, to, isFromConfigServer}] = upgrading;
                _transitions[{upgrading, to, isFromConfigServer}] = to;
            }
            _fcvDocuments[upgrading] = makeFCVDoc(from /* effective */, to /* target */);
        }

        if (FCVParams::kLastLTS != FCVParams::kLastContinuous) {
            // Only config servers may request an upgrade from lastLTS to lastContinuous.
            _transitions[{FCVParams::kLastLTS, FCVParams::kLastContinuous, true}] =
                FCVParams::kUpgradingFromLastLTSToLastContinuous;
            _transitions[{FCVParams::kUpgradingFromLastLTSToLastContinuous,
                          FCVParams::kLastContinuous,
                          true}] = FCVParams::kLastContinuous;
            _fcvDocuments[FCVParams::kUpgradingFromLastLTSToLastContinuous] = makeFCVDoc(
                FCVParams::kLastLTS /* effective */, FCVParams::kLastContinuous /* target */);
        }

        for (auto [downgrading, to] :
             std::vector{std::make_tuple(FCVParams::kDowngradingFromLatestToLastContinuous,
                                         FCVParams::kLastContinuous),
                         std::make_tuple(FCVParams::kDowngradingFromLatestToLastLTS,
                                         FCVParams::kLastLTS)}) {
            for (auto isFromConfigServer : std::vector{false, true}) {
                // Start or complete downgrade from latest.  If this release's lastContinuous ==
                // lastLTS then the second loop iteration just overwrites the first.
                _transitions[{FCVParams::kLatest, to, isFromConfigServer}] = downgrading;
                _transitions[{downgrading, to, isFromConfigServer}] = to;
            }
            _fcvDocuments[downgrading] =
                makeFCVDoc(to /* effective */, to /* target */, FCVParams::kLatest /* previous */
                );
        }
    }

    /**
     * True if a server in ServerGlobalParams::FeatureCompatibility::Version "fromVersion" can
     * transition to "newVersion". Different rules apply if the request is from a config server.
     */
    bool permitsTransition(FCV fromVersion, FCV newVersion, bool isFromConfigServer) const {
        return _transitions.find({fromVersion, newVersion, isFromConfigServer}) !=
            _transitions.end();
    }

    /**
     * Get a feature compatibility version enum value from a document representing the
     * ServerGlobalParams::FeatureCompatibility::Version, or uassert if the document is invalid.
     */
    FCV versionFromFCVDoc(const FeatureCompatibilityVersionDocument& fcvDoc) const {
        auto it = std::find_if(_fcvDocuments.begin(), _fcvDocuments.end(), [&](const auto& value) {
            return value.second == fcvDoc;
        });

        uassert(5147400, "invalid feature compatibility document", it != _fcvDocuments.end());
        return it->first;
    }

    /**
     * Return an FCV representing the transition fromVersion -> newVersion. It may be transitional
     * (such as kUpgradingFromLastLTSToLastContinuous) or not (such as kLastLTS). Different rules
     * apply if the upgrade/downgrade request is from a config server.
     */
    FCV getTransitionalVersion(FCV fromVersion, FCV newVersion, bool isFromConfigServer) const {
        auto it = _transitions.find({fromVersion, newVersion, isFromConfigServer});
        fassert(5147401, it != _transitions.end());
        return it->second;
    }

    /**
     * Get the document representation of a (perhaps transitional) version.
     */
    FeatureCompatibilityVersionDocument getFCVDocument(FCV currentVersion) const {
        auto it = _fcvDocuments.find(currentVersion);
        fassert(5147402, it != _fcvDocuments.end());
        return it->second;
    }

private:
    stdx::unordered_map<FCV, FeatureCompatibilityVersionDocument> _fcvDocuments;

    // Map: (fromVersion, newVersion, isFromConfigServer) -> transitional version.
    stdx::unordered_map<std::tuple<FCV, FCV, bool>, FCV> _transitions;
} fcvTransitions;

bool isWriteableStorageEngine() {
    return !storageGlobalParams.readOnly && (storageGlobalParams.engine != "devnull");
}

// Returns the featureCompatibilityVersion document if it exists.
boost::optional<BSONObj> findFcvDocument(OperationContext* opCtx) {
    // Ensure database is opened and exists.
    AutoGetOrCreateDb autoDb(opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_IX);

    const auto query = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (!swFcv.isOK()) {
        return boost::none;
    }
    return swFcv.getValue();
}

/**
 * Build update command for featureCompatibilityVersion document updates.
 */
void runUpdateCommand(OperationContext* opCtx, const FeatureCompatibilityVersionDocument& fcvDoc) {
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
                fcvDoc.serialize(&updateMods);
            }
            updateSpec.appendBool("upsert", true);
        }
    }
    auto timeout = opCtx->getWriteConcern().usedDefault ? WriteConcernOptions::kNoTimeout
                                                        : opCtx->getWriteConcern().wTimeout;
    auto newWC = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout);
    updateCmd.append(WriteConcernOptions::kWriteConcernField, newWC.toBSON());

    // Update the featureCompatibilityVersion document stored in the server configuration
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.db().toString(), updateCmd.obj(), updateResult);
    uassertStatusOK(getStatusFromWriteCommandReply(updateResult));
}
}  // namespace

void FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
    FCV fromVersion, FCV newVersion, bool isFromConfigServer) {
    uassert(
        5147403,
        "cannot set featureCompatibilityVersion to '{}' while featureCompatibilityVersion is '{}'"_format(
            FCVP::toString(newVersion), FCVP::toString(fromVersion)),
        fcvTransitions.permitsTransition(fromVersion, newVersion, isFromConfigServer));
}

void FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
    OperationContext* opCtx,
    ServerGlobalParams::FeatureCompatibility::Version fromVersion,
    ServerGlobalParams::FeatureCompatibility::Version newVersion,
    bool isFromConfigServer,
    bool setTargetVersion) {

    // Only transition to fully upgraded or downgraded states when we
    // have completed all required upgrade/downgrade behavior.
    auto transitioningVersion = setTargetVersion &&
            serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading(fromVersion)
        ? fromVersion
        : fcvTransitions.getTransitionalVersion(fromVersion, newVersion, isFromConfigServer);
    FeatureCompatibilityVersionDocument fcvDoc =
        fcvTransitions.getFCVDocument(transitioningVersion);
    runUpdateCommand(opCtx, fcvDoc);
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

    FeatureCompatibilityVersionDocument fcvDoc;
    if (storeUpgradeVersion) {
        fcvDoc.setVersion(FCVParams::kLatest);
    } else {
        fcvDoc.setVersion(FCVParams::kLastLTS);
    }

    // We then insert the featureCompatibilityVersion document into the server configuration
    // collection. The server parameter will be updated on commit by the op observer.
    uassertStatusOK(storageInterface->insertDocument(
        opCtx,
        nss,
        repl::TimestampedBSONObj{fcvDoc.toBSON(), Timestamp()},
        repl::OpTime::kUninitializedTerm));  // No timestamp or term because this write is not
                                             // replicated.
}

bool FeatureCompatibilityVersion::isCleanStartUp() {
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    std::vector<std::string> dbNames = storageEngine->listDatabases();

    for (auto&& dbName : dbNames) {
        if (dbName != "local") {
            return false;
        }
    }
    return true;
}

void FeatureCompatibilityVersion::updateMinWireVersion() {
    WireSpec& wireSpec = WireSpec::instance();
    const auto currentFcv = serverGlobalParams.featureCompatibility.getVersion();
    if (currentFcv == FCVParams::kLatest ||
        (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading() &&
         currentFcv != FCVParams::kUpgradingFromLastLTSToLastContinuous)) {
        // FCV == kLatest or FCV is upgrading/downgrading to or from kLatest.
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    } else if (currentFcv == FCVParams::kUpgradingFromLastLTSToLastContinuous ||
               currentFcv == FCVParams::kLastContinuous) {
        // FCV == kLastContinuous or upgrading to kLastContinuous.
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LAST_CONT_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LAST_CONT_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    } else {
        invariant(currentFcv == FCVParams::kLastLTS);
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LAST_LTS_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LAST_LTS_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    }
}

void FeatureCompatibilityVersion::initializeForStartup(OperationContext* opCtx) {
    // Global write lock must be held.
    invariant(opCtx->lockState()->isW());
    auto featureCompatibilityVersion = findFcvDocument(opCtx);
    if (!featureCompatibilityVersion) {
        return;
    }

    // If the server configuration collection already contains a valid featureCompatibilityVersion
    // document, cache it in-memory as a server parameter.
    auto swVersion = FeatureCompatibilityVersionParser::parse(*featureCompatibilityVersion);

    // Note this error path captures all cases of an FCV document existing, but with any
    // unacceptable value. This includes unexpected cases with no path forward such as the FCV value
    // not being a string.
    if (!swVersion.isOK()) {
        uassertStatusOK({ErrorCodes::MustDowngrade,
                         str::stream()
                             << "UPGRADE PROBLEM: Found an invalid featureCompatibilityVersion "
                                "document (ERROR: "
                             << swVersion.getStatus()
                             << "). If the current featureCompatibilityVersion is below 4.4, see "
                                "the documentation on upgrading at "
                             << feature_compatibility_version_documentation::kUpgradeLink << "."});
    }

    auto version = swVersion.getValue();
    serverGlobalParams.mutableFeatureCompatibility.setVersion(version);
    FeatureCompatibilityVersion::updateMinWireVersion();

    // On startup, if the version is in an upgrading or downgrading state, print a warning.
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
        LOGV2_WARNING_OPTIONS(
            4978301,
            {logv2::LogTag::kStartupWarnings},
            "A featureCompatibilityVersion upgrade/downgrade did not complete. To fix this, use "
            "the setFeatureCompatibilityVersion command to resume the upgrade/downgrade",
            "currentfeatureCompatibilityVersion"_attr =
                FeatureCompatibilityVersionParser::toString(version));
    }
}

// Fatally asserts if the featureCompatibilityVersion document is not initialized, when required.
void FeatureCompatibilityVersion::fassertInitializedAfterStartup(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);
    const auto replProcess = repl::ReplicationProcess::get(opCtx);
    const auto& replSettings = repl::ReplicationCoordinator::get(opCtx)->getSettings();

    // The node did not complete the last initial sync. If the initial sync flag is set and we are
    // part of a replica set, we expect the version to be initialized as part of initial sync after
    // startup.
    bool needInitialSync = replSettings.usingReplSets() && replProcess &&
        replProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx);
    if (needInitialSync) {
        return;
    }

    auto fcvDocument = findFcvDocument(opCtx);

    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto dbNames = storageEngine->listDatabases();
    bool nonLocalDatabases = std::any_of(dbNames.begin(), dbNames.end(), [](auto name) {
        return name != NamespaceString::kLocalDb;
    });

    // Fail to start up if there is no featureCompatibilityVersion document and there are non-local
    // databases present.
    if (!fcvDocument && nonLocalDatabases) {
        LOGV2_FATAL_NOTRACE(40652,
                            "Unable to start up mongod due to missing featureCompatibilityVersion "
                            "document. Please run with --repair to restore the document.");
    }

    // If we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the in-memory
    // featureCompatibilityVersion parameter to still be uninitialized until after startup.
    if (isWriteableStorageEngine() && (!replSettings.usingReplSets() || nonLocalDatabases)) {
        invariant(serverGlobalParams.featureCompatibility.isVersionInitialized());
    }
}

/**
 * Read-only server parameter for featureCompatibilityVersion.
 */
// No ability to specify 'none' as set_at type,
// so use 'startup' in the IDL file, then override to none here.
FeatureCompatibilityVersionParameter::FeatureCompatibilityVersionParameter(StringData name,
                                                                           ServerParameterType)
    : ServerParameter(ServerParameterSet::getGlobal(), name, false, false) {}

void FeatureCompatibilityVersionParameter::append(OperationContext* opCtx,
                                                  BSONObjBuilder& b,
                                                  const std::string& name) {
    uassert(ErrorCodes::UnknownFeatureCompatibilityVersion,
            str::stream() << name << " is not yet known.",
            serverGlobalParams.featureCompatibility.isVersionInitialized());

    BSONObjBuilder featureCompatibilityVersionBuilder(b.subobjStart(name));
    auto version = serverGlobalParams.featureCompatibility.getVersion();
    FeatureCompatibilityVersionDocument fcvDoc = fcvTransitions.getFCVDocument(version);
    featureCompatibilityVersionBuilder.appendElements(fcvDoc.toBSON().removeField("_id"));
}

Status FeatureCompatibilityVersionParameter::setFromString(const std::string&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << "."};
}

}  // namespace mongo
