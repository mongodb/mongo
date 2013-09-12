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
#include "mongo/s/type_config_version.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongo::str::stream;

    /**
     * Upgrade v0 to v5 described here
     *
     * This upgrade takes the config server from empty to an initial version.
     */
    bool doUpgradeV0ToV5(const ConnectionString& configLoc,
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

        // If the cluster has not previously been initialized, we need to set the version before
        // using so subsequent mongoses use the config data the same way.  This requires all three
        // config servers online initially.
        try {
            ScopedDbConnection conn(configLoc, 30);
            conn->update(VersionType::ConfigNS, BSON("_id" << 1), versionInfo.toBSON(), true);
            _checkGLE(conn);
            conn.done();
        }
        catch (const DBException& e) {

            *errMsg = stream() << "error writing initial config version" << causedBy(e);

            return false;
        }

        return true;
    }

}
