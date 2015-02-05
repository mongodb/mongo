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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/s/config_upgrade.h"

#include "mongo/client/connpool.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/config_upgrade_helpers.h"
#include "mongo/s/type_locks.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::list;
    using std::string;
    using std::vector;

    static const char* minMongoProcessVersion = "2.6";

    static const char* cannotCleanupMessage =
            "\n\n"
            "******\n"
            "Cannot upgrade config database from v5 to v6 because a previous upgrade\n"
            "failed in the critical section.  Manual intervention is required to re-sync\n"
            "the config servers.\n"
            "******\n";

    namespace {
        /**
         * Returns false if the { ts: 1 } unique index does not exist. Returns true if it does
         * or cannot confirm that it does due to errors.
         */
        bool hasBadIndex(const ConnectionString& configLoc,
                         string* errMsg) {
            const BSONObj lockIdxKey = BSON(LocksType::lockID() << 1);
            const NamespaceString indexNS(LocksType::ConfigNS);

            vector<HostAndPort> configHosts = configLoc.getServers();
            for (vector<HostAndPort>::const_iterator configIter = configHosts.begin();
                    configIter != configHosts.end(); ++configIter) {

                list<BSONObj> indexSpecs;
                try {
                    ScopedDbConnection conn(*configIter);
                    indexSpecs = conn->getIndexSpecs(indexNS);
                    conn.done();
                }
                catch (const DBException& ex) {
                    *errMsg = str::stream() << "error while checking { ts: 1 } index"
                                            << causedBy(ex);
                    return true;
                }

                for (list<BSONObj>::const_iterator idxIter = indexSpecs.begin();
                        idxIter != indexSpecs.end(); ++idxIter) {
                    BSONObj indexSpec(*idxIter);
                    if (indexSpec["key"].Obj().woCompare(lockIdxKey) == 0) {
                        if (indexSpec["unique"].trueValue()) {
                            *errMsg = str::stream() << "unique { ts: 1 } index still exists in "
                                                    << configIter->toString();
                            return true;
                        }
                    }
                }
            }

            return false;
        }
    }

    /**
     * Upgrades v5 to v6.
     */
    bool doUpgradeV5ToV6(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        verify(lastVersionInfo.getCurrentVersion() == UpgradeHistory_DummyBumpPre2_6);
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

        // Make sure the { ts: 1 } index is not unique by dropping the existing one
        // and rebuilding the index with the right specification.

        const BSONObj lockIdxKey = BSON(LocksType::lockID() << 1);
        const NamespaceString indexNS(LocksType::ConfigNS);

        bool dropOk = false;
        try {
            ScopedDbConnection conn(configLoc);
            BSONObj dropResponse;
            dropOk = conn->runCommand(indexNS.db().toString(),
                                      BSON("dropIndexes" << indexNS.coll()
                                           << "index" << lockIdxKey),
                                      dropResponse);
            conn.done();
        }
        catch (const DBException& ex) {
            if (ex.getCode() == 13105) {
                // 13105 is the exception code from SyncClusterConnection::findOne that gets
                // thrown when one of the command responses has an "ok" field that is not true.
                dropOk = false;
            }
            else {
                *errMsg = str::stream() << "Failed to drop { ts: 1 } index" << causedBy(ex);
                return false;
            }
        }

        if (!dropOk && hasBadIndex(configLoc, errMsg)) {
            // Fail only if the index still exists.
            return false;
        }

        result = clusterCreateIndex(LocksType::ConfigNS,
                                    BSON(LocksType::lockID() << 1),
                                    false, // unique
                                    WriteConcernOptions::AllConfigs,
                                    NULL);

        if (!result.isOK()) {
            *errMsg = str::stream() << "error while creating { ts: 1 } index on config db"
                                    << causedBy(result);
            return false;
        }

        LOG(1) << "Checking to make sure that the right { ts: 1 } index is created...";

        if (hasBadIndex(configLoc, errMsg)) {
            return false;
        }

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
