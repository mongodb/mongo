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
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"

namespace mongo {

class BSONObj;
class OperationContext;

class FeatureCompatibilityVersion {
public:
    /**
     * Should be taken in shared mode by any operations that should not run while
     * setFeatureCompatibilityVersion is running.
     *
     * setFCV takes this lock in exclusive mode so that it both does not run with the shared mode
     * operations and does not run with itself.
     */
    static Lock::ResourceMutex fcvLock;

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
     * uassert that a transition from fromVersion to newVersion is permitted. Different rules apply
     * if the request is from a config server.
     */
    static void validateSetFeatureCompatibilityVersionRequest(
        ServerGlobalParams::FeatureCompatibility::Version fromVersion,
        ServerGlobalParams::FeatureCompatibility::Version newVersion,
        bool isFromConfigServer);

    /**
     * Updates the on-disk feature compatibility version document for the transition fromVersion ->
     * newVersion. This is required to be a valid transition.
     */
    static void updateFeatureCompatibilityVersionDocument(
        OperationContext* opCtx,
        ServerGlobalParams::FeatureCompatibility::Version fromVersion,
        ServerGlobalParams::FeatureCompatibility::Version newVersion,
        bool isFromConfigServer,
        bool setTargetVersion);

    /**
     * If there are no non-local databases, store the featureCompatibilityVersion document. If we
     * are not running with --shardsvr, set the version to be the upgrade value. If we are running
     * with --shardsvr, set the version to be the downgrade value.
     */
    static void setIfCleanStartup(OperationContext* opCtx,
                                  repl::StorageInterface* storageInterface);

    /**
     * Returns true if the server is on a clean startup. A clean startup means there are no
     * databases on disk besides the local database.
     */
    static bool isCleanStartUp();

    /**
     * Sets the server's outgoing and incomingInternalClient minWireVersions according to the
     * current featureCompatibilityVersion value.
     */
    static void updateMinWireVersion();
};

/**
 * Utility class to prevent the FCV from changing while the FixedFCVRegion is in scope.
 */
class FixedFCVRegion {
public:
    explicit FixedFCVRegion(OperationContext* opCtx) {
        invariant(!opCtx->lockState()->isLocked());
        _lk.emplace(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);
    }

    ~FixedFCVRegion() = default;

    void release() {
        _lk.reset();
    }

private:
    boost::optional<Lock::SharedLock> _lk;
};

}  // namespace mongo
