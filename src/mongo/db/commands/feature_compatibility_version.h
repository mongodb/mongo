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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version_document_gen.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"

namespace mongo {

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
     * Returns the on-disk feature compatibility version document if it exists.
     */
    static boost::optional<BSONObj> findFeatureCompatibilityVersionDocument(
        OperationContext* opCtx);

    /**
     * uassert that a transition from fromVersion to newVersion is permitted. Different rules apply
     * if the request is from a config server.
     */
    static void validateSetFeatureCompatibilityVersionRequest(
        OperationContext* opCtx,
        const SetFeatureCompatibilityVersion& setFCVRequest,
        ServerGlobalParams::FeatureCompatibility::Version fromVersion);

    /**
     * Updates the on-disk feature compatibility version document for the transition fromVersion ->
     * newVersion. This is required to be a valid transition.
     */
    static void updateFeatureCompatibilityVersionDocument(
        OperationContext* opCtx,
        ServerGlobalParams::FeatureCompatibility::Version fromVersion,
        ServerGlobalParams::FeatureCompatibility::Version newVersion,
        bool isFromConfigServer,
        boost::optional<Timestamp> timestamp,
        bool setTargetVersion);

    /**
     * If we are in clean startup (the server has no replicated collections), store the
     * featureCompatibilityVersion document. If we are not running with --shardsvr, set the version
     * to be the upgrade value. If we are running with --shardsvr, set the version to be the
     * downgrade value.
     */
    static void setIfCleanStartup(OperationContext* opCtx,
                                  repl::StorageInterface* storageInterface);

    /**
     * Returns true if the server has no replicated collections.
     */
    static bool hasNoReplicatedCollections(OperationContext* opCtx);

    /**
     * Sets the server's outgoing and incomingInternalClient minWireVersions according to the
     * current featureCompatibilityVersion value.
     */
    static void updateMinWireVersion();

    /**
     * Returns a scoped object, which holds the 'fcvLock' in exclusive mode for the given scope. It
     * must only be used by the setFeatureCompatibilityVersion command in order to serialise with
     * concurrent 'FixedFCVRegions'.
     */
    static Lock::ExclusiveLock enterFCVChangeRegion(OperationContext* opCtx);

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
 * Utility class to prevent the FCV from changing while the FixedFCVRegion is in scope.
 */
class FixedFCVRegion {
public:
    explicit FixedFCVRegion(OperationContext* opCtx);
    ~FixedFCVRegion();

    bool operator==(const ServerGlobalParams::FeatureCompatibility::Version& other) const;
    bool operator!=(const ServerGlobalParams::FeatureCompatibility::Version& other) const;

    const ServerGlobalParams::FeatureCompatibility& operator*() const;
    const ServerGlobalParams::FeatureCompatibility* operator->() const;

private:
    Lock::SharedLock _lk;
};

}  // namespace mongo
