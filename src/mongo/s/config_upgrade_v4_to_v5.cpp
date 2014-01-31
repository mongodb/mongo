/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/config_upgrade.h"

#include "mongo/client/connpool.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/config_upgrade_helpers.h"

namespace mongo {

    static const char* minMongoProcessVersion = "2.4";

    static const char* cannotCleanupMessage =
            "\n\n"
            "******\n"
            "Cannot upgrade config database from v4 to v5 because a previous upgrade\n"
            "failed in the critical section.  Manual intervention is required to re-sync\n"
            "the config servers.  See:\n"
            // TODO: verify release note link
            "http://docs.mongodb.org/manual/release-notes/2dot6upgradenotes/\n"
            "******\n";

    /**
     * Upgrades v4 to v5.
     */
    bool doUpgradeV4ToV5(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        verify(lastVersionInfo.getCurrentVersion() == UpgradeHistory_MandatoryEpochVersion);
        Status result = preUpgradeCheck(configLoc, lastVersionInfo, minMongoProcessVersion);

        if (!result.isOK()) {
            if (result.code() == ErrorCodes::ManualInterventionRequired) {
                *errMsg = cannotCleanupMessage;
            }
            else {
                *errMsg = result.toString();
            }

            return false;
        }

        // This is not needed because we are not actually going to make any modifications
        // on the other collections in the config server for this particular upgrade.
        // startConfigUpgrade(configLoc.toString(),
        //                    lastVersionInfo.getCurrentVersion(),
        //                    OID::gen());

        // If we actually need to modify something in the config servers these need to follow
        // after calling startConfigUpgrade(...):
        //
        // 1. Acquire necessary locks.
        // 2. Make a backup of the collections we are about to modify.
        // 3. Perform the upgrade process on the backup collection.
        // 4. Verify that no changes were made to the collections since the backup was performed.
        // 5. Call enterConfigUpgradeCriticalSection(configLoc.toString(),
        //    lastVersionInfo.getCurrentVersion()).
        // 6. Rename the backup collection to the name of the original collection with
        //    dropTarget set to true.

        // We're only after the version bump in commitConfigUpgrade here since we never
        // get into the critical section.
        Status commitStatus = commitConfigUpgrade(configLoc.toString(),
                                                  lastVersionInfo.getCurrentVersion(),
                                                  MIN_COMPATIBLE_CONFIG_VERSION,
                                                  CURRENT_CONFIG_VERSION);

        if (!commitStatus.isOK()) {
            *errMsg = commitStatus.toString();
            return false;
        }

        return true;
    }
}
