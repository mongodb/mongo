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
 */

#include "mongo/s/config_upgrade.h"

#include "mongo/client/connpool.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/config_upgrade_helpers.h"

namespace mongo {

    static const char* minMongoProcessVersion = "2.4";

    // We want to keep this around, at the same time we don't want the compiler to complain
    // about unused variable.
#if 0
    static const char* cleanupMessage =
            "\n\n"
            "******\n"
            "Did not upgrade config database from v4 to v5 because the upgrade failed in\n"
            "the critical section.  Manual intervention is required to re-sync the config\n"
            "servers.  See:\n"
            "http://dochub.mongodb.org/core/2dot6upgradenotes\n"
            "******\n";
#endif

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

        verify(lastVersionInfo.getCurrentVersion() == UpgradeHistory_StrictEpochVersion);
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

        // TODO: grab necessary locks
        // TODO: backup collections
        // TODO: perform upgrade on the backup.
        // TODO: perform switch from backup to actual.

        return true;
    }
}
