// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <type_traits>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

struct ResolvedFCVTransition {
    /**
     * Represents the resolved transitional FCV based on the actual and the requested version.
     *
     * Example - New FCV upgrade:
     *   actualVersion: "8.0", requestedVersion: "8.3"
     *   => Resolved transitionalVersion: "upgrading from 8.0 to 8.3"
     *
     * Example - Resume interrupted FCV upgrade:
     *   actualVersion: "upgrading from 8.0 to 8.3", requestedVersion: "8.3"
     *   => Resolved transitionalVersion: "upgrading from 8.0 to 8.3"
     *
     * Example - Return to original FCV after failed FCV upgrade ("upgrading to downgrading"):
     *   actualVersion: "upgrading from 8.0 to 8.3", requestedVersion: "8.0"
     *   => Resolved transitionalVersion: "downgrading from 8.3 to 8.0"
     */
    multiversion::FeatureCompatibilityVersion transitionalVersion;

    // Range of phases [startPhase, endPhase] to run.
    SetFCVPhaseEnum startPhase;
    SetFCVPhaseEnum endPhase;

    bool shouldRun(SetFCVPhaseEnum phase) {
        return startPhase <= phase && phase <= endPhase;
    }

    // Timestamp for the FCV transition. Only generated in sharded clusters for replay protection.
    boost::optional<Timestamp> changeTimestamp;
};

class FeatureCompatibilityVersion {
public:
    /**
     * Reads the featureCompatibilityVersion (FCV) document in admin.system.version and initializes
     * the FCV global state. Returns an error if the FCV document exists and is invalid. Does not
     * return an error if it is missing. This should be checked after startup with
     * fassertInitializedAfterStartup.
     *
     * Throws a MustDowngrade error if an existing FCV document contains an invalid version.
     */
    static void initializeForStartup(OperationContext* opCtx);

    /**
     * Fatally asserts if the featureCompatibilityVersion is not properly initialized after startup.
     */
    static void fassertInitializedAfterStartup(OperationContext* opCtx);

    /**
     * Performs actions that need to be done after startup, including asserting if FCV is not
     * properly initialized and adding transitions for FCV states
     */
    static void afterStartupActions(OperationContext* opCtx);

    /**
     * Returns the on-disk feature compatibility version document if it can be found.
     * If there was an error finding the document, returns the error reason.
     */
    static StatusWith<BSONObj> findFeatureCompatibilityVersionDocument(OperationContext* opCtx);

    /**
     * uassert that a transition from fromVersion to newVersion is permitted.
     * Some transitions can only be done if the request is from a config server.
     */
    static ResolvedFCVTransition validateSetFeatureCompatibilityVersionRequest(
        OperationContext* opCtx,
        const SetFeatureCompatibilityVersion& setFCVRequest,
        multiversion::FeatureCompatibilityVersion fromVersion);

    /**
     * Updates the on-disk feature compatibility version document to the given version.
     * `version` may be a transitional or non-transitional FCV.
     *
     * Holds the 'fcvLock' in exclusive mode for the duration of the update, serialising with
     * concurrent 'FixedFCVRegions'. If provided, 'withFCVLockHeld' runs under that lock before the
     * document is written, letting the caller perform checks that must be atomic with the update.
     */
    static void updateFeatureCompatibilityVersionDocument(
        OperationContext* opCtx,
        multiversion::FeatureCompatibilityVersion version,
        boost::optional<SetFCVPhaseEnum> phase,
        boost::optional<Timestamp> timestamp,
        boost::optional<bool> setIsCleaningServerMetadata,
        unique_function<void()> withFCVLockHeld = {});

    /**
     * If we are in clean startup (the server has no replicated collections), store the
     * featureCompatibilityVersion document. If we are not running with --shardsvr, set the version
     * to be the upgrade value. If we are running with --shardsvr, set the version to be the
     * downgrade value.
     *
     * If 'term' is provided, writes FCV with a timestamp and replicates it in oplog.
     * Returns FCV's Timestamp.
     */
    static Timestamp setIfCleanStartup(
        OperationContext* opCtx,
        repl::StorageInterface* storageInterface,
        const multiversion::FeatureCompatibilityVersion& minimumRequiredFCV,
        long long term = repl::OpTime::kUninitializedTerm);

    /**
     * Returns true if the server has no replicated collections.
     */
    static bool hasNoReplicatedCollections(OperationContext* opCtx);

    /**
     * Sets the server's outgoing and incomingInternalClient minWireVersions according to the
     * current featureCompatibilityVersion value.
     */
    static void updateMinWireVersion(OperationContext* opCtx);

    /**
     * Used by the FCV OpObserver to set the timestamp of the last opTime where the FCV was updated.
     * We use this to ensure the user does not see a non-transitional FCV that is not in the
     * majority snapshot, since upgrading or downgrading will not work in that circumstance.
     */
    static void advanceLastFCVUpdateTimestamp(Timestamp fcvUpdateTimestamp);

    /**
     * Used by the FCV OpObserver at rollback time.  The rollback FCV is always in the
     * majority snapshot, so it is safe to clear the lastFCVUpdateTimestamp then.
     *
     * Also used in rare cases when the replication coordinator majority snapshot is cleared.
     */
    static void clearLastFCVUpdateTimestamp();
};

/**
 * Utility class to prevent the on-disk FCV from changing while the FixedFCVRegion is in scope.
 *
 * Note that this does not prevent the in-memory FCV from changing (which for example could be reset
 * during initial sync). The operator* and operator-> functions return a MutableFCV, which could
 * change at different points in time, so if you wanted to get a consistent snapshot of the
 * in-memory FCV, you should still use the ServerGlobalParams::MutableFCV's acquireFCVSnapshot()
 * function.
 */
class FixedFCVRegion {
public:
    explicit FixedFCVRegion(OperationContext* opCtx);
    ~FixedFCVRegion();

    FixedFCVRegion(FixedFCVRegion&&) = default;

    bool operator==(const multiversion::FeatureCompatibilityVersion& other) const;
    bool operator!=(const multiversion::FeatureCompatibilityVersion& other) const;

    const ServerGlobalParams::MutableFCV& operator*() const;
    const ServerGlobalParams::MutableFCV* operator->() const;

private:
    Lock::SharedLock _lk;
};

/*
 * Optimistically runs the specified checks over a stable (fully upgraded / fully downgraded) FCV.
 * This is intended for commands such as `validate` or `checkMetadataConsistency` to check the
 * metadata is consistent with FCV, avoiding both acquiring locks and false positives.
 * Returns boost::none if a concurrent upgrade/downgrade happened during the check.
 */
template <typename Fn>
auto tryCheckUnderStableFCV(OperationContext* opCtx, Fn&& checkFn)
    -> boost::optional<std::invoke_result_t<Fn, ServerGlobalParams::FCVSnapshot>> {
    // Without Symmetric FCV, the FCV document may show the fully upgraded/downgraded state
    // but there may still be metadata changes being done by setFCV.
    if (!gFeatureFlagSymmetricFCV.isEnabled()) {
        return boost::none;
    }

    // TODO SERVER-130577: Generalize changeTimestamp to replica sets to lift this restriction.
    tassert(12797701,
            "tryCheckUnderStableFCV currently only supports sharded clusters",
            serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

    auto readFCVDocument = [&] {
        return FeatureCompatibilityVersionDocument::parse(uassertStatusOK(
            FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(opCtx)));
    };

    const auto initialFCVDocument = readFCVDocument();
    if (initialFCVDocument.getTargetVersion()) {
        // We are upgrading or downgrading, so the check can not be reliably run.
        return boost::none;
    }

    auto result = checkFn(ServerGlobalParams::FCVSnapshot(initialFCVDocument.getVersion()));

    if (readFCVDocument() != initialFCVDocument) {
        // An upgrade or downgrade happened during the check, so discard to avoid false positives.
        // Note that the FCV document includes a `changeTimestamp`, so we will correctly discard the
        // result even if a full downgrade + full upgrade cycle happened across the check.
        return boost::none;
    }

    return result;
}

}  // namespace mongo
