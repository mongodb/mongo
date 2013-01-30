/**
 *    Copyright (C) 2012 10gen Inc.
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
#include "mongo/s/type_config_version.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongo::str::stream;

    /**
     * Upgrade v0 to v4 described here
     *
     * This upgrade takes the config server from empty to an initial version.
     */
    bool doUpgradeV0ToV4(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        verify(lastVersionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion);

        //
        // Even though the initial config write is a single-document update, that single document
        // is on multiple config servers and requests can interleave.  The upgrade lock prevents
        // this.
        //

        log() << "writing initial config version at v" << CURRENT_CONFIG_VERSION << endl;

        OID newClusterId = OID::gen();

        VersionType versionInfo;

        // Upgrade to new version
        versionInfo.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
        versionInfo.setCurrentVersion(CURRENT_CONFIG_VERSION);
        versionInfo.setClusterId(newClusterId);

        verify(versionInfo.isValid(NULL));

        scoped_ptr<ScopedDbConnection> connPtr;

        // If the cluster has not previously been initialized, we need to set the version before
        // using so subsequent mongoses use the config data the same way.  This requires all three
        // config servers online initially.
        try {
            connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            conn->update(VersionType::ConfigNS, BSON("_id" << 1), versionInfo.toBSON(), true);
            _checkGLE(conn);
        }
        catch (const DBException& e) {

            *errMsg = stream() << "error writing initial config version" << causedBy(e);

            return false;
        }

        connPtr->done();
        return true;
    }

}
