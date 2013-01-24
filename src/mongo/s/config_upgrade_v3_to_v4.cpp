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

#include "mongo/base/owned_pointer_map.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/connpool.h"
#include "mongo/client/distlock.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/config_upgrade_helpers.h"
#include "mongo/s/field_parser.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_config_version.h"
#include "mongo/s/type_shard.h"

namespace mongo {

    using mongo::str::stream;

    // Failed critical section upgrade message
    static const char* cleanupMessage =
            "\n\n"
            "******\n"
            "Did not upgrade config database from v3 to v4 because the upgrade failed in\n"
            "the critical section.  Manual intervention is required to re-sync the config\n"
            "servers.  See:\n"
            "http://docs.mongodb.org/manual/release-notes/2.4/\n"
            "******\n";

    static const char* cannotCleanupMessage =
            "\n\n"
            "******\n"
            "Cannot upgrade config database from v3 to v4 because a previous upgrade\n"
            "failed in the critical section.  Manual intervention is required to re-sync\n"
            "the config servers.  See:\n"
            "http://docs.mongodb.org/manual/release-notes/2.4/\n"
            "******\n";

    static const char* minMongoProcessVersion = "2.2";

    // Custom field used in upgrade state to determine if/where we failed on last upgrade
    const BSONField<bool> inCriticalSectionField(string("inCriticalSection"), false);

    bool _cleanupUpgradeState(const ConnectionString& configLoc,
                              const OID& lastUpgradeId,
                              string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        scoped_ptr<ScopedDbConnection> connPtr;

        string workingSuffix = genWorkingSuffix(lastUpgradeId);

        // Create new collection
        try {
            connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            bool resultOk;
            BSONObj dropResult;

            resultOk = conn->dropCollection(CollectionType::ConfigNS + workingSuffix, &dropResult);

            if (!resultOk) {

                *errMsg = stream() << "could not drop collection "
                                   << (CollectionType::ConfigNS + workingSuffix)
                                   << causedBy(dropResult.toString());

                return false;
            }

            resultOk = conn->dropCollection(ChunkType::ConfigNS + workingSuffix, &dropResult);

            if (!resultOk) {

                *errMsg = stream() << "could not drop collection "
                                   << (ChunkType::ConfigNS + workingSuffix)
                                   << causedBy(dropResult.toString());

                return false;
            }
        }
        catch (const DBException& e) {

            *errMsg = stream() << "could not drop collections during cleanup of upgrade "
                               << lastUpgradeId << causedBy(e);

            return false;
        }

        connPtr->done();
        return true;
    }

    /**
     * Upgrade v3 to v4 described here.
     *
     * This upgrade takes a config server without collection epochs (potentially) and adds
     * epochs to all mongo processes.
     *
     */
    bool doUpgradeV3ToV4(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        verify(lastVersionInfo.getCurrentVersion() == UpgradeHistory_NoEpochVersion);

        if (lastVersionInfo.isUpgradeIdSet() && lastVersionInfo.getUpgradeId().isSet()) {

            //
            // Another upgrade failed, so cleanup may be necessary
            //

            BSONObj lastUpgradeState = lastVersionInfo.getUpgradeState();

            bool inCriticalSection;
            if (!FieldParser::extract(lastUpgradeState,
                                      inCriticalSectionField,
                                      &inCriticalSection,
                                      errMsg))
            {

                *errMsg = stream() << "problem reading previous upgrade state" << causedBy(errMsg);

                return false;
            }

            if (inCriticalSection) {

                // Manual intervention is needed here.  Somehow our upgrade didn't get applied
                // consistently across config servers.

                *errMsg = cannotCleanupMessage;

                return false;
            }

            if (!_cleanupUpgradeState(configLoc, lastVersionInfo.getUpgradeId(), errMsg)) {
                
                // If we can't cleanup the old upgrade state, the user might have done it for us,
                // not a fatal problem (we'll just end up with extra collections).
                
                warning() << "could not cleanup previous upgrade state" << causedBy(errMsg) << endl;
                *errMsg = "";
            }
        }

        //
        // Check the versions of other mongo processes in the cluster before upgrade.
        // We can't upgrade if there are active pre-v2.2 processes in the cluster
        //

        Status mongoVersionStatus = checkClusterMongoVersions(configLoc,
                                                              string(minMongoProcessVersion));

        if (!mongoVersionStatus.isOK()) {

            *errMsg = stream() << "cannot upgrade with pre-v" << minMongoProcessVersion
                               << " mongo processes active in the cluster"
                               << causedBy(mongoVersionStatus);

            return false;
        }

        VersionType newVersionInfo;
        lastVersionInfo.cloneTo(&newVersionInfo);

        // Set our upgrade id and state
        OID upgradeId = OID::gen();
        newVersionInfo.setUpgradeId(upgradeId);
        newVersionInfo.setUpgradeState(BSONObj());

        // Write our upgrade id and state
        {
            scoped_ptr<ScopedDbConnection> connPtr;

            try {
                connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                ScopedDbConnection& conn = *connPtr;

                verify(newVersionInfo.isValid(NULL));

                conn->update(VersionType::ConfigNS,
                             BSON("_id" << 1 << VersionType::version_DEPRECATED(3)),
                             newVersionInfo.toBSON());
                _checkGLE(conn);
            }
            catch (const DBException& e) {

                *errMsg = stream() << "could not initialize version info for upgrade"
                                   << causedBy(e);

                return false;
            }

            connPtr->done();
        }

        //
        // First lock all collection namespaces that exist
        //

        OwnedPointerMap<string, CollectionType> ownedCollections;
        const map<string, CollectionType*>& collections = ownedCollections.map();

        Status findCollectionsStatus = findAllCollectionsV3(configLoc, &ownedCollections);

        if (!findCollectionsStatus.isOK()) {

            *errMsg = stream() << "could not read collections from config server"
                               << causedBy(findCollectionsStatus);

            return false;
        }

        //
        // Acquire locks for all sharded collections
        // Something that didn't involve getting thousands of locks would be better.
        //

        OwnedPointerVector<ScopedDistributedLock> collectionLocks;

        log() << "acquiring locks for " << collections.size() << " sharded collections..." << endl;

        for (map<string, CollectionType*>::const_iterator it = collections.begin();
                it != collections.end(); ++it)
        {
            const CollectionType& collection = *(it->second);

            ScopedDistributedLock* namespaceLock = new ScopedDistributedLock(configLoc,
                                                                             collection.getNS());

            namespaceLock->setLockMessage(str::stream() << "upgrading " << collection.getNS()
                                                        << " with new epochs for upgrade "
                                                        << upgradeId);

            if (!namespaceLock->acquire(15 * 60 * 1000, errMsg)) {

                *errMsg = stream() << "could not acquire all namespace locks for upgrade"
                                   << causedBy(errMsg);

                return false;
            }

            collectionLocks.mutableVector().push_back(namespaceLock);

            // Progress update
            if (collectionLocks.vector().size() % 10 == 0) {
                log() << "acquired " << collectionLocks.vector().size() << " locks out of "
                      << collections.size() << " for config upgrade" << endl;
            }
        }

        // We are now preventing all splits and migrates for all sharded collections

        // Get working and backup suffixes
        string workingSuffix = genWorkingSuffix(upgradeId);
        string backupSuffix = genBackupSuffix(upgradeId);

        log() << "copying collection and chunk metadata to working and backup collections..."
              << endl;

        // Get a backup and working copy of the config.collections and config.chunks collections

        Status copyStatus = copyFrozenCollection(configLoc,
                                                 CollectionType::ConfigNS,
                                                 CollectionType::ConfigNS + workingSuffix);

        if (!copyStatus.isOK()) {

            *errMsg = stream() << "could not copy " << CollectionType::ConfigNS << " to "
                               << (CollectionType::ConfigNS + workingSuffix)
                               << causedBy(copyStatus);

            return false;
        }

        copyStatus = copyFrozenCollection(configLoc,
                                          CollectionType::ConfigNS,
                                          CollectionType::ConfigNS + backupSuffix);

        if (!copyStatus.isOK()) {

            *errMsg = stream() << "could not copy " << CollectionType::ConfigNS << " to "
                               << (CollectionType::ConfigNS + backupSuffix) << causedBy(copyStatus);

            return false;
        }

        copyStatus = copyFrozenCollection(configLoc,
                                          ChunkType::ConfigNS,
                                          ChunkType::ConfigNS + workingSuffix);

        if (!copyStatus.isOK()) {

            *errMsg = stream() << "could not copy " << ChunkType::ConfigNS << " to "
                               << (ChunkType::ConfigNS + workingSuffix) << causedBy(copyStatus);

            return false;
        }

        copyStatus = copyFrozenCollection(configLoc,
                                          ChunkType::ConfigNS,
                                          ChunkType::ConfigNS + backupSuffix);

        if (!copyStatus.isOK()) {

            *errMsg = stream() << "could not copy " << ChunkType::ConfigNS << " to "
                               << (ChunkType::ConfigNS + backupSuffix) << causedBy(copyStatus);

            return false;
        }

        //
        // Go through sharded collections one-by-one and add epochs where missing
        //

        for (map<string, CollectionType*>::const_iterator it = collections.begin();
                it != collections.end(); ++it)
        {
            // Create a copy so that we can change the epoch later
            CollectionType collection;
            it->second->cloneTo(&collection);

            log() << "checking epochs for " << collection.getNS() << " collection..." << endl;

            OID epoch = collection.getEpoch();

            //
            // Go through chunks to find epoch if we haven't found it or to verify epoch is the same
            //

            OwnedPointerVector<ChunkType> ownedChunks;
            const vector<ChunkType*>& chunks = ownedChunks.vector();

            Status findChunksStatus = findAllChunks(configLoc, collection.getNS(), &ownedChunks);

            if (!findChunksStatus.isOK()) {

                *errMsg = stream() << "could not read chunks from config server"
                                   << causedBy(findChunksStatus);

                return false;
            }

            for (vector<ChunkType*>::const_iterator chunkIt = chunks.begin();
                    chunkIt != chunks.end(); ++chunkIt)
            {
                const ChunkType& chunk = *(*chunkIt);

                // If our chunk epoch is set and doesn't match
                if (epoch.isSet() && chunk.getVersion().epoch().isSet()
                    && chunk.getVersion().epoch() != epoch)
                {

                    *errMsg = stream() << "chunk epoch for " << chunk.toString() << " in "
                                       << collection.getNS() << " does not match found epoch "
                                       << epoch;

                    return false;
                }
                else if (!epoch.isSet() && chunk.getVersion().epoch().isSet()) {
                    epoch = chunk.getVersion().epoch();
                }
            }

            //
            // Write collection epoch if needed
            //

            if (!collection.getEpoch().isSet()) {

                OID newEpoch = OID::gen();

                log() << "writing new epoch " << newEpoch << " for " << collection.getNS()
                      << " collection..." << endl;

                scoped_ptr<ScopedDbConnection> connPtr;

                try {
                    connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                    ScopedDbConnection& conn = *connPtr;

                    conn->update(CollectionType::ConfigNS + workingSuffix,
                                 BSON(CollectionType::ns(collection.getNS())),
                                 BSON("$set" << BSON(CollectionType::DEPRECATED_lastmodEpoch(newEpoch))));
                    _checkGLE(conn);
                }
                catch (const DBException& e) {

                    *errMsg = stream() << "could not write a new epoch for " << collection.getNS()
                                       << causedBy(e);

                    return false;
                }

                connPtr->done();
                collection.setEpoch(newEpoch);
            }

            epoch = collection.getEpoch();
            verify(epoch.isSet());

            //
            // Now write verified epoch to all chunks
            //

            log() << "writing epoch " << epoch << " for " << chunks.size() << " chunks in "
                  << collection.getNS() << " collection..." << endl;

            {
                scoped_ptr<ScopedDbConnection> connPtr;

                try {
                    connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                    ScopedDbConnection& conn = *connPtr;

                    // Multi-update of all chunks
                    conn->update(ChunkType::ConfigNS + workingSuffix,
                                 BSON(ChunkType::ns(collection.getNS())),
                                 BSON("$set" << BSON(ChunkType::DEPRECATED_epoch(epoch))),
                                 false,
                                 true); // multi
                    _checkGLE(conn);
                }
                catch (const DBException& e) {

                    *errMsg = stream() << "could not write a new epoch " << epoch.toString()
                                       << " for chunks in " << collection.getNS() << causedBy(e);

                    return false;
                }

                connPtr->done();
            }
        }

        //
        // Paranoid verify the collection writes
        //

        {
            scoped_ptr<ScopedDbConnection> connPtr;

            try {
                connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                ScopedDbConnection& conn = *connPtr;

                // Find collections with no epochs
                BSONObj emptyDoc =
                        conn->findOne(CollectionType::ConfigNS + workingSuffix,
                                      BSON("$unset" << BSON(CollectionType::DEPRECATED_lastmodEpoch() << 1)));

                if (!emptyDoc.isEmpty()) {

                    *errMsg = stream() << "collection " << emptyDoc
                                       << " is still missing epoch after config upgrade";

                    connPtr->done();
                    return false;
                }

                // Find collections with empty epochs
                emptyDoc = conn->findOne(CollectionType::ConfigNS + workingSuffix,
                                         BSON(CollectionType::DEPRECATED_lastmodEpoch(OID())));

                if (!emptyDoc.isEmpty()) {

                    *errMsg = stream() << "collection " << emptyDoc
                                       << " still has empty epoch after config upgrade";

                    connPtr->done();
                    return false;
                }

                // Find chunks with no epochs
                emptyDoc =
                        conn->findOne(ChunkType::ConfigNS + workingSuffix,
                                      BSON("$unset" << BSON(ChunkType::DEPRECATED_epoch() << 1)));

                if (!emptyDoc.isEmpty()) {

                    *errMsg = stream() << "chunk " << emptyDoc
                                       << " is still missing epoch after config upgrade";

                    connPtr->done();
                    return false;
                }

                // Find chunks with empty epochs
                emptyDoc = conn->findOne(ChunkType::ConfigNS + workingSuffix,
                                         BSON(ChunkType::DEPRECATED_epoch(OID())));

                if (!emptyDoc.isEmpty()) {

                    *errMsg = stream() << "chunk " << emptyDoc
                                       << " still has empty epoch after config upgrade";

                    connPtr->done();
                    return false;
                }
            }
            catch (const DBException& e) {

                *errMsg = stream() << "could not verify epoch writes" << causedBy(e);

                return false;
            }

            connPtr->done();
        }

        //
        // Double check that our collections haven't changed
        //

        Status idCheckStatus = checkIdsTheSame(configLoc,
                                               CollectionType::ConfigNS,
                                               CollectionType::ConfigNS + workingSuffix);

        if (!idCheckStatus.isOK()) {

            *errMsg = stream() << CollectionType::ConfigNS
                               << " was modified while working on upgrade"
                               << causedBy(idCheckStatus);

            return false;
        }

        idCheckStatus = checkIdsTheSame(configLoc,
                                        ChunkType::ConfigNS,
                                        ChunkType::ConfigNS + workingSuffix);

        if (!idCheckStatus.isOK()) {

            *errMsg = stream() << ChunkType::ConfigNS << " was modified while working on upgrade"
                               << causedBy(idCheckStatus);

            return false;
        }

        //
        // ENTER CRITICAL SECTION
        //

        newVersionInfo.setUpgradeState(BSON(inCriticalSectionField(true)));

        {
            scoped_ptr<ScopedDbConnection> connPtr;

            try {
                connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                ScopedDbConnection& conn = *connPtr;

                verify(newVersionInfo.isValid(NULL));

                conn->update(VersionType::ConfigNS,
                             BSON("_id" << 1 << VersionType::version_DEPRECATED(3)),
                             newVersionInfo.toBSON());
                _checkGLE(conn);
            }
            catch (const DBException& e) {

                // No cleanup message here since we're not sure if we wrote or not, and
                // not dangerous either way except to prevent further updates (at which point
                // the message is printed)
                *errMsg = stream()
                        << "could not update version info to enter critical update section"
                        << causedBy(e);

                return false;
            }

            // AT THIS POINT ANY FAILURE REQUIRES MANUAL INTERVENTION!
            connPtr->done();
        }

        Status overwriteStatus = overwriteCollection(configLoc,
                                                     CollectionType::ConfigNS + workingSuffix,
                                                     CollectionType::ConfigNS);

        if (!overwriteStatus.isOK()) {

            error() << cleanupMessage << endl;
            *errMsg = stream() << "could not overwrite collection " << CollectionType::ConfigNS
                               << " with working collection "
                               << (CollectionType::ConfigNS + workingSuffix)
                               << causedBy(overwriteStatus);

            return false;
        }

        overwriteStatus = overwriteCollection(configLoc,
                                              ChunkType::ConfigNS + workingSuffix,
                                              ChunkType::ConfigNS);

        if (!overwriteStatus.isOK()) {

            error() << cleanupMessage << endl;
            *errMsg = stream() << "could not overwrite collection " << ChunkType::ConfigNS
                               << " with working collection "
                               << (ChunkType::ConfigNS + workingSuffix)
                               << causedBy(overwriteStatus);

            return false;
        }

        //
        // Finally update the version to latest and add clusterId to version
        //

        OID newClusterId = OID::gen();

        // Note: hardcoded versions, since this is a very particular upgrade
        // Note: DO NOT CLEAR the config version unless bumping the minCompatibleVersion,
        // we want to save the excludes that were set.

        newVersionInfo.setMinCompatibleVersion(UpgradeHistory_NoEpochVersion);
        newVersionInfo.setCurrentVersion(UpgradeHistory_MandatoryEpochVersion);
        newVersionInfo.setClusterId(newClusterId);

        // Leave critical section
        newVersionInfo.setUpgradeId(OID());
        newVersionInfo.setUpgradeState(BSONObj());

        log() << "writing new version info and clusterId " << newClusterId << "..." << endl;

        {
            scoped_ptr<ScopedDbConnection> connPtr;

            try {
                connPtr.reset(ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
                ScopedDbConnection& conn = *connPtr;

                verify(newVersionInfo.isValid(NULL));

                conn->update(VersionType::ConfigNS,
                             BSON("_id" << 1 << VersionType::version_DEPRECATED(UpgradeHistory_NoEpochVersion)),
                             newVersionInfo.toBSON());
                _checkGLE(conn);
            }
            catch (const DBException& e) {

                error() << cleanupMessage << endl;
                *errMsg = stream() << "could not write new version info "
                                   << "and exit critical upgrade section" << causedBy(e);

                return false;
            }

            connPtr->done();
        }

        //
        // END CRITICAL SECTION
        //

        return true;
    }

}
