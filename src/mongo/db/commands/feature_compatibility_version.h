/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"

namespace mongo {

class BSONObj;
class OperationContext;

/**
 * Startup parameter to ignore featureCompatibilityVersion checks. This parameter cannot be set if
 * the node is started with --replSet, --master, or --slave. This should never be set by end users.
 */
extern bool internalValidateFeaturesAsMaster;

class FeatureCompatibilityVersion {
public:
    static constexpr StringData kCollection = "admin.system.version"_sd;
    static constexpr StringData kCommandName = "setFeatureCompatibilityVersion"_sd;
    static constexpr StringData kDatabase = "admin"_sd;
    static constexpr StringData kParameterName = "featureCompatibilityVersion"_sd;
    static constexpr StringData kVersionField = "version"_sd;
    static constexpr StringData kTargetVersionField = "targetVersion"_sd;

    /**
     * Should be taken in exclusive mode by any operations that should not run while
     * setFeatureCompatibilityVersion is running.
     */
    static Lock::ResourceMutex fcvLock;

    /**
     * Parses the featureCompatibilityVersion document from admin.system.version, and returns the
     * state represented by the combination of the targetVersion and version.
     */
    static StatusWith<ServerGlobalParams::FeatureCompatibility::Version> parse(
        const BSONObj& featureCompatibilityVersionDoc);

    static StringData toString(ServerGlobalParams::FeatureCompatibility::Version version) {
        switch (version) {
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36:
                return FeatureCompatibilityVersionCommandParser::kVersion36;
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34:
                return FeatureCompatibilityVersionCommandParser::kVersion34;
            case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36:
                return FeatureCompatibilityVersionCommandParser::kVersionUpgradingTo36;
            case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34:
                return FeatureCompatibilityVersionCommandParser::kVersionDowngradingTo34;
            case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault34Behavior:
                return FeatureCompatibilityVersionCommandParser::kVersionUnset;
            default:
                MONGO_UNREACHABLE;
        }
    }

    /**
     * Records intent to perform a 3.4 -> 3.6 upgrade by updating the on-disk feature
     * compatibility version document to have 'version'=3.4, 'targetVersion'=3.6.
     * Should be called before schemas are modified.
     */
    static void setTargetUpgrade(OperationContext* opCtx);

    /**
     * Records intent to perform a 3.6 -> 3.4 downgrade by updating the on-disk feature
     * compatibility version document to have 'version'=3.4, 'targetVersion'=3.4.
     * Should be called before schemas are modified.
     */
    static void setTargetDowngrade(OperationContext* opCtx);

    /**
     * Records the completion of a 3.4 <-> 3.6 upgrade or downgrade by updating the on-disk
     * feature compatibility version document to have 'version'=version and unsetting the
     * 'targetVersion' field.
     * Should be called after schemas are modified.
     */
    static void unsetTargetUpgradeOrDowngrade(OperationContext* opCtx, StringData version);

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
     * Examines a document inserted or updated in admin.system.version. If it is the
     * featureCompatibilityVersion document, validates the document and on commit, updates
     * the server parameter.
     */
    static void onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc);

    /**
     * Examines the _id of a document removed from admin.system.version. If it is the
     * featureCompatibilityVersion document, resets the server parameter to its default value
     * on commit.
     */
    static void onDelete(OperationContext* opCtx, const BSONObj& doc);

    /**
     * Resets the server parameter to its default value on commit.
     */
    static void onDropCollection(OperationContext* opCtx);

    /**
     * Sets the server's outgoing and incomingInternalClient minWireVersions according to the
     * current featureCompatibilityVersion value.
     */
    static void updateMinWireVersion();

private:
    /**
     * Validate version. Uasserts if invalid.
     */
    static void _validateVersion(StringData version);

    /**
     * Build update command.
     */
    typedef stdx::function<void(BSONObjBuilder)> UpdateBuilder;
    static void _runUpdateCommand(OperationContext* opCtx, UpdateBuilder callback);
};


}  // namespace mongo
