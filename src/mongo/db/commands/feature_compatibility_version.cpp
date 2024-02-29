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


#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using repl::UnreplicatedWritesBlock;
using GenericFCV = multiversion::GenericFCV;
using FCV = multiversion::FeatureCompatibilityVersion;

using namespace fmt::literals;

namespace {

/**
 * Utility class for recording permitted transitions between feature compatibility versions and
 * their on-disk representation as FeatureCompatibilityVersionDocument objects.
 */
// TODO (SERVER-74847): Add back 'const' qualifier to FCVTransitions class declaration
class FCVTransitions {
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
             std::vector{GenericFCV::kLastLTS, GenericFCV::kLastContinuous, GenericFCV::kLatest}) {
            _fcvDocuments[fcv] = makeFCVDoc(fcv);
        }

        for (auto [from, upgrading, to] :
             std::vector{std::make_tuple(GenericFCV::kLastLTS,
                                         GenericFCV::kUpgradingFromLastLTSToLatest,
                                         GenericFCV::kLatest),
                         std::make_tuple(GenericFCV::kLastContinuous,
                                         GenericFCV::kUpgradingFromLastContinuousToLatest,
                                         GenericFCV::kLatest)}) {
            for (auto&& isFromConfigServer : {false, true}) {
                // Start or complete upgrading to latest. If this release's lastContinuous ==
                // lastLTS then the second loop iteration just overwrites the first.
                _transitions[{from, to, isFromConfigServer}] = upgrading;
                _transitions[{upgrading, to, isFromConfigServer}] = to;
            }
            _fcvDocuments[upgrading] = makeFCVDoc(from /* effective */, to /* target */);
        }

        if (GenericFCV::kLastLTS != GenericFCV::kLastContinuous) {
            // Only config servers may request an upgrade from lastLTS to lastContinuous.
            _transitions[{GenericFCV::kLastLTS, GenericFCV::kLastContinuous, true}] =
                GenericFCV::kUpgradingFromLastLTSToLastContinuous;
            _transitions[{GenericFCV::kUpgradingFromLastLTSToLastContinuous,
                          GenericFCV::kLastContinuous,
                          true}] = GenericFCV::kLastContinuous;
            _fcvDocuments[GenericFCV::kUpgradingFromLastLTSToLastContinuous] = makeFCVDoc(
                GenericFCV::kLastLTS /* effective */, GenericFCV::kLastContinuous /* target */);
        }

        for (auto&& isFromConfigServer : {false, true}) {
            _transitions[{GenericFCV::kLatest, GenericFCV::kLastLTS, isFromConfigServer}] =
                GenericFCV::kDowngradingFromLatestToLastLTS;
            _transitions[{GenericFCV::kDowngradingFromLatestToLastLTS,
                          GenericFCV::kLastLTS,
                          isFromConfigServer}] = GenericFCV::kLastLTS;

            // Add transition from downgrading -> upgrading.
            _transitions[{GenericFCV::kDowngradingFromLatestToLastLTS,
                          GenericFCV::kLatest,
                          isFromConfigServer}] = GenericFCV::kUpgradingFromLastLTSToLatest;
        }
        _fcvDocuments[GenericFCV::kDowngradingFromLatestToLastLTS] =
            makeFCVDoc(GenericFCV::kLastLTS /* effective */,
                       GenericFCV::kLastLTS /* target */,
                       GenericFCV::kLatest /* previous */
            );
    }

    // TODO (SERVER-74847): Remove this transition once we remove testing around
    // downgrading from latest to last continuous.
    void addTransitionFromLatestToLastContinuous() {
        for (auto&& isFromConfigServer : {false, true}) {
            _transitions[{GenericFCV::kLatest, GenericFCV::kLastContinuous, isFromConfigServer}] =
                GenericFCV::kDowngradingFromLatestToLastContinuous;
            _transitions[{GenericFCV::kDowngradingFromLatestToLastContinuous,
                          GenericFCV::kLastContinuous,
                          isFromConfigServer}] = GenericFCV::kLastContinuous;
        }

        FeatureCompatibilityVersionDocument fcvDoc;
        fcvDoc.setVersion(GenericFCV::kLastContinuous);
        fcvDoc.setTargetVersion(GenericFCV::kLastContinuous);
        fcvDoc.setPreviousVersion(GenericFCV::kLatest);

        _fcvDocuments[GenericFCV::kDowngradingFromLatestToLastContinuous] = fcvDoc;
    }

    /**
     * True if a server in multiversion::FeatureCompatibilityVersion "fromVersion" can
     * transition to "newVersion". Different rules apply if the request is from a config server.
     */
    bool permitsTransition(FCV fromVersion, FCV newVersion, bool isFromConfigServer) const {
        return _transitions.find({fromVersion, newVersion, isFromConfigServer}) !=
            _transitions.end();
    }

    /**
     * Get a feature compatibility version enum value from a document representing the
     * multiversion::FeatureCompatibilityVersion, or uassert if the document is invalid.
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

/**
 * Taken in shared mode by any operations that need to ensure that the FCV does not change during
 * its execution.
 *
 * setFCV takes this lock in exclusive mode when changing the FCV value.
 */
Lock::ResourceMutex fcvDocumentLock("featureCompatibilityVersionDocumentLock");
// lastFCVUpdateTimestamp contains the latest oplog entry timestamp which updated the FCV.
// It is reset on rollback.
Timestamp lastFCVUpdateTimestamp;
SimpleMutex lastFCVUpdateTimestampMutex;

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
                queryFilter.append("_id", multiversion::kParameterName);
            }
            {
                BSONObjBuilder updateMods(updateSpec.subobjStart("u"));
                fcvDoc.serialize(&updateMods);
            }
            updateSpec.appendBool("upsert", true);
        }
    }
    auto timeout = opCtx->getWriteConcern().isImplicitDefaultWriteConcern()
        ? WriteConcernOptions::kNoTimeout
        : opCtx->getWriteConcern().wTimeout;
    auto newWC = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout);
    updateCmd.append(WriteConcernOptions::kWriteConcernField, newWC.toBSON());

    // Update the featureCompatibilityVersion document stored in the server configuration
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.dbName(), updateCmd.obj(), updateResult);
    uassertStatusOK(getStatusFromWriteCommandReply(updateResult));
}

}  // namespace

StatusWith<BSONObj> FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(
    OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IX);
    invariant(autoColl.ensureDbExists(opCtx),
              redactTenant(NamespaceString::kServerConfigurationNamespace));

    const auto query = BSON("_id" << multiversion::kParameterName);
    return repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
}

void FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
    OperationContext* opCtx, const SetFeatureCompatibilityVersion& setFCVRequest, FCV fromVersion) {

    auto newVersion = setFCVRequest.getCommandParameter();
    auto isFromConfigServer = setFCVRequest.getFromConfigServer().value_or(false);

    uassert(
        5147403,
        "cannot set featureCompatibilityVersion to '{}' while featureCompatibilityVersion is '{}'"_format(
            multiversion::toString(newVersion), multiversion::toString(fromVersion)),
        fcvTransitions.permitsTransition(fromVersion, newVersion, isFromConfigServer));

    auto fcvObj = findFeatureCompatibilityVersionDocument(opCtx);
    if (!fcvObj.isOK()) {
        Status status = fcvObj.getStatus();
        invariant(ErrorCodes::isRetriableError(status));
        uasserted(
            8531600,
            str::stream() << "failed to validate setFCV request because the existing FCV document "
                             "could not be found due to error: "
                          << status.toString() << ". Retry the setFCV request.");
    }

    auto fcvDoc = FeatureCompatibilityVersionDocument::parse(
        IDLParserContext("featureCompatibilityVersionDocument"), fcvObj.getValue());

    auto isCleaningServerMetadata = fcvDoc.getIsCleaningServerMetadata();
    uassert(7428200,
            "Cannot upgrade featureCompatibilityVersion if a previous FCV downgrade stopped in the "
            "middle of cleaning up internal server metadata. Retry the FCV downgrade until it "
            "succeeds before attempting to upgrade the FCV.",
            !(newVersion > fromVersion &&
              (isCleaningServerMetadata.is_initialized() && *isCleaningServerMetadata)));

    auto setFCVPhase = setFCVRequest.getPhase();
    if (!isFromConfigServer || !setFCVPhase) {
        return;
    }

    auto changeTimestamp = setFCVRequest.getChangeTimestamp();
    invariant(changeTimestamp);
    auto previousTimestamp = fcvDoc.getChangeTimestamp();

    if (setFCVPhase == SetFCVPhaseEnum::kStart) {
        uassert(
            5563501,
            "Shard received a timestamp for phase 1 of the 'setFeatureCompatibilityVersion' "
            "command which is too old, so the request is discarded. This may indicate that the "
            "request is related to a previous invocation of the 'setFeatureCompatibilityVersion' "
            "command which, for example, was temporarily stuck on network.",
            !previousTimestamp || previousTimestamp <= changeTimestamp);
    } else {
        uassert(5563601,
                "Cannot transition to fully upgraded or fully downgraded state if the shard is not "
                "in kUpgrading or kDowngrading state",
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                    .isUpgradingOrDowngrading());

        tassert(5563502,
                "Shard received a request for phase 2 of the 'setFeatureCompatibilityVersion' "
                "command that cannot be correlated with the previous one, so the request is "
                "discarded. This may indicate that the request for step 1 was not received or "
                "processed properly.",
                previousTimestamp);

        uassert(5563503,
                "Shard received a timestamp for phase 2 of the 'setFeatureCompatibilityVersion' "
                "command that does not match the one received for phase 1, so the request is "
                "discarded. This could indicate, for example, that the request is related to a "
                "previous invocation of the 'setFeatureCompatibilityVersion' command which, for "
                "example, was temporarily stuck on network.",
                previousTimestamp == changeTimestamp);
    }
}

void FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
    OperationContext* opCtx,
    FCV fromVersion,
    FCV newVersion,
    bool isFromConfigServer,
    boost::optional<Timestamp> changeTimestamp,
    bool setTargetVersion,
    boost::optional<bool> setIsCleaningServerMetadata) {

    // We may have just stepped down, in which case we should not proceed.
    opCtx->checkForInterrupt();

    // Only transition to fully upgraded or downgraded states when we have completed all required
    // upgrade/downgrade behavior, unless it is the newly added downgrading to upgrading path.
    auto transitioningVersion = setTargetVersion &&
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isUpgradingOrDowngrading(
                fromVersion) &&
            !(fromVersion == GenericFCV::kDowngradingFromLatestToLastLTS &&
              newVersion == GenericFCV::kLatest)
        ? fromVersion
        : fcvTransitions.getTransitionalVersion(fromVersion, newVersion, isFromConfigServer);

    // Create the new FCV document that we want to replace the FCV document to.
    FeatureCompatibilityVersionDocument newFCVDoc =
        fcvTransitions.getFCVDocument(transitioningVersion);

    newFCVDoc.setChangeTimestamp(changeTimestamp);

    // The setIsCleaningServerMetadata parameter can either be true, false, or boost::none.
    // True indicates we want to set the isCleaningServerMetadata FCV document field to true.
    // False indicates we want to remove the isCleaningServerMetadata FCV document field.
    // boost::none indicates that we don't want to change the current value of the
    // isCleaningServerMetadata field.
    if (setIsCleaningServerMetadata.is_initialized()) {
        // True case: set isCleaningServerMetadata doc field to true.
        if (*setIsCleaningServerMetadata) {
            newFCVDoc.setIsCleaningServerMetadata(true);
        }
        // Else, false case: don't set the isCleaningServerMetadata field in newFCVDoc. This is
        // because runUpdateCommand overrides the current whole FCV document with what is in
        // newFCVDoc so not setting the field is effectively removing it.
    } else {
        // boost::none case:
        // If we don't want to update the isCleaningServerMetadata, we need to make sure not to
        // override the existing field if it exists, so get the current isCleaningServerMetadata
        // field value from the current FCV document and set it in newFCVDoc.
        // This is to protect against the case where a previous FCV downgrade failed
        // in the isCleaningServerMetadata phase, and the user runs setFCV again. In that
        // case we do not want to remove the existing isCleaningServerMetadata FCV doc field
        // because it would not be safe to upgrade the FCV.
        auto currentFCVObj = findFeatureCompatibilityVersionDocument(opCtx);
        if (!currentFCVObj.isOK()) {
            Status status = currentFCVObj.getStatus();
            invariant(ErrorCodes::isRetriableError(status));
            uasserted(8531601,
                      str::stream()
                          << "failed to update FCV document because the existing FCV document "
                             "could not be found due to error: "
                          << status.toString() << ". Retry the setFCV request.");
        }

        auto currentFCVDoc = FeatureCompatibilityVersionDocument::parse(
            IDLParserContext("featureCompatibilityVersionDocument"), currentFCVObj.getValue());

        auto currentIsCleaningServerMetadata = currentFCVDoc.getIsCleaningServerMetadata();
        if (currentIsCleaningServerMetadata.is_initialized() && *currentIsCleaningServerMetadata) {
            newFCVDoc.setIsCleaningServerMetadata(*currentIsCleaningServerMetadata);
        }
    }

    // Replace the current FCV document with newFCVDoc.
    runUpdateCommand(opCtx, newFCVDoc);
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* opCtx,
                                                    repl::StorageInterface* storageInterface) {
    if (!hasNoReplicatedCollections(opCtx)) {
        if (!gDefaultStartupFCV.empty()) {
            LOGV2(7557701,
                  "Ignoring the provided defaultStartupFCV parameter since the FCV already exists");
        }
        return;
    }


    // If the server was not started with --shardsvr, the default featureCompatibilityVersion on
    // clean startup is the upgrade version. If it was started with --shardsvr, the default
    // featureCompatibilityVersion is the downgrade version, so that it can be safely added to a
    // downgrade version cluster. The config server will run setFeatureCompatibilityVersion as
    // part of addShard.
    const bool storeUpgradeVersion =
        !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer);

    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
    NamespaceString nss(NamespaceString::kServerConfigurationNamespace);

    {
        CollectionOptions options;
        options.uuid = UUID::gen();
        uassertStatusOK(storageInterface->createCollection(opCtx, nss, options));
    }

    // Set FCV to lastLTS for nodes started with --shardsvr. If an FCV was specified at startup
    // through a startup parameter, set it to that FCV. Otherwise, set it to latest.
    FeatureCompatibilityVersionDocument fcvDoc;
    if (!storeUpgradeVersion) {
        fcvDoc.setVersion(GenericFCV::kLastLTS);
    } else if (!gDefaultStartupFCV.empty()) {
        StringData versionString = StringData(gDefaultStartupFCV);
        FCV parsedVersion;

        if (versionString == multiversion::toString(GenericFCV::kLastLTS)) {
            parsedVersion = GenericFCV::kLastLTS;
        } else if (versionString == multiversion::toString(GenericFCV::kLastContinuous)) {
            parsedVersion = GenericFCV::kLastContinuous;
        } else if (versionString == multiversion::toString(GenericFCV::kLatest)) {
            parsedVersion = GenericFCV::kLatest;
        } else {
            parsedVersion = GenericFCV::kLatest;
            LOGV2_WARNING_OPTIONS(7557700,
                                  {logv2::LogTag::kStartupWarnings},
                                  "The provided 'defaultStartupFCV' is not a valid FCV. Setting "
                                  "the FCV to the latest FCV instead",
                                  "defaultStartupFCV"_attr = versionString,
                                  "latestFCV"_attr = multiversion::toString(GenericFCV::kLatest));
        }

        fcvDoc.setVersion(parsedVersion);
    } else {
        fcvDoc.setVersion(GenericFCV::kLatest);
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

bool FeatureCompatibilityVersion::hasNoReplicatedCollections(OperationContext* opCtx) {
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    std::vector<DatabaseName> dbNames = storageEngine->listDatabases();
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& dbName : dbNames) {
        Lock::DBLock dbLock(opCtx, dbName, MODE_S);
        for (auto&& collNss : catalog->getAllCollectionNamesFromDb(opCtx, dbName)) {
            if (collNss.isReplicated()) {
                return false;
            }
        }
    }
    return true;
}

void FeatureCompatibilityVersion::updateMinWireVersion(OperationContext* opCtx) {
    WireSpec& wireSpec = WireSpec::getWireSpec(opCtx->getServiceContext());
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const auto currentFcv = fcvSnapshot.getVersion();
    // The reason we set the minWireVersion to LATEST_WIRE_VERSION when downgrading from latest as
    // well as on upgrading to latest is because we shouldn’t decrease the minWireVersion until we
    // have fully downgraded to the lower FCV in case we get any backwards compatibility breakages,
    // since during `kDowngradingFrom_X_to_Y` we may still be stopping/cleaning up any features from
    // the upgraded FCV. In essence, a node with the upgraded FCV/binary should not be able to
    // communicate with downgraded binary nodes until the FCV is completely downgraded to
    // `kVersion_Y`.
    if (currentFcv == GenericFCV::kLatest ||
        (fcvSnapshot.isUpgradingOrDowngrading() &&
         currentFcv != GenericFCV::kUpgradingFromLastLTSToLastContinuous)) {
        // FCV == kLatest or FCV is upgrading/downgrading to or from kLatest.
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    } else if (currentFcv == GenericFCV::kUpgradingFromLastLTSToLastContinuous ||
               currentFcv == GenericFCV::kLastContinuous) {
        // FCV == kLastContinuous or upgrading to kLastContinuous.
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LAST_CONT_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LAST_CONT_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    } else {
        invariant(currentFcv == GenericFCV::kLastLTS);
        WireSpec::Specification newSpec = *wireSpec.get();
        newSpec.incomingInternalClient.minWireVersion = LAST_LTS_WIRE_VERSION;
        newSpec.outgoing.minWireVersion = LAST_LTS_WIRE_VERSION;
        wireSpec.reset(std::move(newSpec));
    }
}

void FeatureCompatibilityVersion::initializeForStartup(OperationContext* opCtx) {
    // Global write lock must be held.
    invariant(shard_role_details::getLocker(opCtx)->isW());
    auto featureCompatibilityVersion = findFeatureCompatibilityVersionDocument(opCtx);
    if (!featureCompatibilityVersion.isOK()) {
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().logFCVWithContext(
            "startup"_sd);
        return;
    }

    // If the server configuration collection already contains a valid
    // featureCompatibilityVersion document, cache it in-memory as a server parameter.
    auto swVersion =
        FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion.getValue());

    // Note this error path captures all cases of an FCV document existing, but with any
    // unacceptable value. This includes unexpected cases with no path forward such as the FCV
    // value not being a string.
    if (!swVersion.isOK()) {
        uassertStatusOK({ErrorCodes::MustDowngrade,
                         str::stream()
                             << "UPGRADE PROBLEM: Found an invalid featureCompatibilityVersion "
                                "document (ERROR: "
                             << swVersion.getStatus()
                             << "). If the current featureCompatibilityVersion is below "
                             << multiversion::toString(multiversion::GenericFCV::kLastLTS)
                             << ", see the documentation on upgrading at "
                             << feature_compatibility_version_documentation::kUpgradeLink << "."});
    }

    auto version = swVersion.getValue();
    serverGlobalParams.mutableFCV.setVersion(version);
    FeatureCompatibilityVersion::updateMinWireVersion(opCtx);
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();

    fcvSnapshot.logFCVWithContext("startup"_sd);

    // On startup, if the version is in an upgrading or downgrading state, print a warning.
    if (fcvSnapshot.isUpgradingOrDowngrading()) {
        LOGV2_WARNING_OPTIONS(
            4978301,
            {logv2::LogTag::kStartupWarnings},
            "A featureCompatibilityVersion upgrade/downgrade did not complete. To fix this, "
            "use "
            "the setFeatureCompatibilityVersion command to resume the upgrade/downgrade",
            "currentfeatureCompatibilityVersion"_attr = multiversion::toString(version));
    }
}

// Fatally asserts if the featureCompatibilityVersion document is not initialized, when
// required.
void FeatureCompatibilityVersion::fassertInitializedAfterStartup(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);
    const auto replProcess = repl::ReplicationProcess::get(opCtx);
    const bool usingReplication =
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();

    // The node did not complete the last initial sync. If the initial sync flag is set and we
    // are part of a replica set, we expect the version to be initialized as part of initial
    // sync after startup.
    bool needInitialSync = usingReplication && replProcess &&
        replProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx);
    if (needInitialSync) {
        return;
    }

    auto fcvDocument = findFeatureCompatibilityVersionDocument(opCtx);

    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto dbNames = storageEngine->listDatabases();
    bool nonLocalDatabases = std::any_of(
        dbNames.begin(), dbNames.end(), [](auto dbName) { return dbName != DatabaseName::kLocal; });

    // Fail to start up if there is no featureCompatibilityVersion document and there are
    // non-local databases present.
    if (!fcvDocument.isOK() && nonLocalDatabases) {
        LOGV2_FATAL_NOTRACE(40652,
                            "Unable to start up mongod due to missing featureCompatibilityVersion "
                            "document. Please run with --repair to restore the document.");
    }

    // If we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the
    // in-memory featureCompatibilityVersion parameter to still be uninitialized until after
    // startup. In standalone mode, FCV is initialized during startup, even in read-only mode.
    bool isWriteableStorageEngine = storageGlobalParams.engine != "devnull";
    if (isWriteableStorageEngine && (!usingReplication || nonLocalDatabases)) {
        invariant(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isVersionInitialized());
    }
}

void FeatureCompatibilityVersion::addTransitionFromLatestToLastContinuous() {
    fcvTransitions.addTransitionFromLatestToLastContinuous();
}

Lock::ExclusiveLock FeatureCompatibilityVersion::enterFCVChangeRegion(OperationContext* opCtx) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    return Lock::ExclusiveLock(opCtx, fcvDocumentLock);
}

void FeatureCompatibilityVersion::advanceLastFCVUpdateTimestamp(Timestamp fcvUpdateTimestamp) {
    stdx::lock_guard lk(lastFCVUpdateTimestampMutex);
    if (fcvUpdateTimestamp > lastFCVUpdateTimestamp) {
        lastFCVUpdateTimestamp = fcvUpdateTimestamp;
    }
}

void FeatureCompatibilityVersion::clearLastFCVUpdateTimestamp() {
    stdx::lock_guard lk(lastFCVUpdateTimestampMutex);
    lastFCVUpdateTimestamp = Timestamp();
}


void FeatureCompatibilityVersionParameter::append(OperationContext* opCtx,
                                                  BSONObjBuilder* b,
                                                  StringData name,
                                                  const boost::optional<TenantId>&) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    uassert(ErrorCodes::UnknownFeatureCompatibilityVersion,
            str::stream() << name << " is not yet known.",
            fcvSnapshot.isVersionInitialized());

    BSONObjBuilder featureCompatibilityVersionBuilder(b->subobjStart(name));
    auto version = fcvSnapshot.getVersion();
    FeatureCompatibilityVersionDocument fcvDoc = fcvTransitions.getFCVDocument(version);
    featureCompatibilityVersionBuilder.appendElements(fcvDoc.toBSON().removeField("_id"));
    if (!fcvDoc.getTargetVersion()) {
        // If the FCV has been recently set to the fully upgraded FCV but is not part of the
        // majority snapshot, then if we do a binary upgrade, we may see the old FCV at startup.
        // It is not safe to do oplog application on the new binary at that point.  So we make
        // sure that when we report the FCV, it is in the majority snapshot. (The same
        // consideration applies at downgrade, where if a recently-set fully downgraded FCV is
        // not part of the majority snapshot, the downgraded binary will see the upgrade FCV and
        // fail.)
        const auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet = replCoordinator && replCoordinator->getSettings().isReplSet();
        auto neededMajorityTimestamp = [] {
            stdx::lock_guard lk(lastFCVUpdateTimestampMutex);
            return lastFCVUpdateTimestamp;
        }();
        if (isReplSet && !neededMajorityTimestamp.isNull()) {
            auto status = replCoordinator->awaitTimestampCommitted(opCtx, neededMajorityTimestamp);
            // If majority reads are not supported, we will take a full snapshot on clean
            // shutdown and the new FCV will be included, so upgrade is possible.
            if (status.code() != ErrorCodes::CommandNotSupported)
                uassertStatusOK(
                    status.withContext("Most recent 'featureCompatibilityVersion' was not in the "
                                       "majority snapshot on this node"));
        }
    }
}

Status FeatureCompatibilityVersionParameter::setFromString(StringData,
                                                           const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << "."};
}

FixedFCVRegion::FixedFCVRegion(OperationContext* opCtx)
    : _lk([&] {
          invariant(!shard_role_details::getLocker(opCtx)->isLocked());
          invariant(!shard_role_details::getLocker(opCtx)->isRSTLLocked());
          return Lock::SharedLock(opCtx, fcvDocumentLock);
      }()) {}

FixedFCVRegion::~FixedFCVRegion() = default;

// Note that the FixedFCVRegion only prevents the on-disk FCV from changing, not
// the in-memory FCV. (which for example could be reset during initial sync). The operator* and
// operator-> functions return a MutableFCV, which could change at different points in time. If you
// wanted to get a consistent snapshot of the in-memory FCV, you should still use the
// ServerGlobalParams::MutableFCV's acquireFCVSnapshot() function to get a FCVSnapshot.
const ServerGlobalParams::MutableFCV& FixedFCVRegion::operator*() const {
    return serverGlobalParams.featureCompatibility;
}

const ServerGlobalParams::MutableFCV* FixedFCVRegion::operator->() const {
    return &serverGlobalParams.featureCompatibility;
}

bool FixedFCVRegion::operator==(const FCV& other) const {
    return serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion() == other;
}

bool FixedFCVRegion::operator!=(const FCV& other) const {
    return !(*this == other);
}

}  // namespace mongo
