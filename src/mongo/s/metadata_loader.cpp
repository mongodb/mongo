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

#include "mongo/s/metadata_loader.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientmockcursor.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_manager.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"

namespace mongo {

    /**
     * This is an adapter so we can use config diffs - mongos and mongod do them slightly
     * differently.
     *
     * The mongod adapter here tracks only a single shard, and stores ranges by (min, max).
     */
    class SCMConfigDiffTracker : public ConfigDiffTracker<BSONObj,string> {
    public:
        SCMConfigDiffTracker(const string& currShard) : _currShard(currShard) {}

        virtual bool isTracked(const BSONObj& chunkDoc) const {
            return chunkDoc["shard"].type() == String && chunkDoc["shard"].String() == _currShard;
        }

        virtual BSONObj maxFrom(const BSONObj& val) const {
            return val;
        }

        virtual pair<BSONObj,BSONObj> rangeFor(const BSONObj& chunkDoc,
                                               const BSONObj& min,
                                               const BSONObj& max) const {
            return make_pair(min, max);
        }

        virtual string shardFor(const string& name) const {
            return name;
        }

        virtual string nameFrom(const string& shard) const {
            return shard;
        }

        string _currShard;
    };

    //
    // MetadataLoader implementation
    //

    MetadataLoader::MetadataLoader(ConnectionString configLoc) : _configLoc(configLoc) { }

    MetadataLoader::~MetadataLoader() { }

    CollectionManager* MetadataLoader::makeCollectionManager(const string& ns,
                                                             const string& shard,
                                                             const CollectionManager* oldManager,
                                                             string* errMsg) {
        // The error message string is optional.
        string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        auto_ptr<CollectionManager> manager(new CollectionManager);
        if (initCollection(ns, shard, oldManager, manager.get(), errMsg)) {
            if (manager->getNumChunks() > 0) {
                dassert(manager->isValid());
            }

            return manager.release();
        }

        return NULL;
    }

    CollectionManager* MetadataLoader::makeEmptyCollectionManager() {
        CollectionManager* manager = new CollectionManager;
        manager->_maxCollVersion = ChunkVersion(1, 0, OID());
        manager->_maxShardVersion = ChunkVersion(1, 0, OID());
        dassert(manager->isValid());
        return manager;
    }

    bool MetadataLoader::initCollection(const string& ns,
                                        const string& shard,
                                        const CollectionManager* oldManager,
                                        CollectionManager* manager,
                                        string* errMsg) {
        //
        // Bring collection entry from the config server.
        //

        BSONObj collObj;
        {
            scoped_ptr<ScopedDbConnection> connPtr;

            try {
                connPtr.reset(
                    ScopedDbConnection::getInternalScopedDbConnection(_configLoc.toString(), 30));
                ScopedDbConnection& conn = *connPtr;

                collObj = conn->findOne(CollectionType::ConfigNS, QUERY(CollectionType::ns()<<ns));
            }
            catch (const DBException& e) {
                *errMsg = str::stream() << "caught exception accessing the config servers "
                                        << causedBy(e);

                // We deliberately do not return connPtr to the pool, since it was involved
                // with the error here.

                return false;
            }

            connPtr->done();
        }

        CollectionType collDoc;
        if (!collDoc.parseBSON(collObj, errMsg) || !collDoc.isValid(errMsg)) {
            return false;
        }

        //
        // Load or generate default chunks for collection config.
        //

        if (!collDoc.getKeyPattern().isEmpty()) {

            manager->_key = collDoc.getKeyPattern();

            if(!initChunks(collDoc, ns, shard, oldManager, manager, errMsg)){
                return false;
            }
        }
        else if(collDoc.getPrimary() == shard) {

            if (shard == "") {
                warning() << "shard not verified, assuming collection "
                          << ns << " is unsharded on this shard" << endl;
            }

            manager->_key = BSONObj();
            manager->_maxShardVersion = ChunkVersion(1, 0, collDoc.getEpoch());
            manager->_maxCollVersion = manager->_maxShardVersion;
        }
        else {
            *errMsg = str::stream() << "collection " << ns << " does not have a shard key "
                                    << "and primary " << collDoc.getPrimary()
                                    << " does not match this shard " << shard;
            return false;
        }

        return true;
    }

    bool MetadataLoader::initChunks(const CollectionType& collDoc,
                                    const string& ns,
                                    const string& shard,
                                    const CollectionManager* oldManager,
                                    CollectionManager* manager,
                                    string* errMsg) {

        map<string,ChunkVersion> versionMap;
        manager->_maxCollVersion = ChunkVersion(0, 0, collDoc.getEpoch());

        // Check to see if we should use the old version or not.
        if (oldManager) {

            ChunkVersion oldVersion = oldManager->getMaxShardVersion();

            if (oldVersion.isSet() && oldVersion.hasCompatibleEpoch(collDoc.getEpoch())) {

                // Our epoch for coll version and shard version should be the same.
                verify(oldManager->getMaxCollVersion().hasCompatibleEpoch(collDoc.getEpoch()));

                versionMap[shard] = oldManager->_maxShardVersion;
                manager->_maxCollVersion = oldManager->_maxCollVersion;

                // TODO: This could be made more efficient if copying not required, but
                // not as frequently reloaded as in mongos.
                manager->_chunksMap = oldManager->_chunksMap;

                LOG(2) << "loading new chunks for collection " << ns
                       << " using old chunk manager w/ version "
                       << oldManager->getMaxShardVersion()
                       << " and " << manager->_chunksMap.size() << " chunks" << endl;
            }
        }

        // Exposes the new 'manager's range map and version to the "differ," who
        // would ultimately be responsible of filling them up.
        SCMConfigDiffTracker differ(shard);
        differ.attach(ns, manager->_chunksMap, manager->_maxCollVersion, versionMap);

        try {

            scoped_ptr<ScopedDbConnection> connPtr(
                ScopedDbConnection::getInternalScopedDbConnection(_configLoc.toString(), 30));
            ScopedDbConnection& conn = *connPtr;

            auto_ptr<DBClientCursor> cursor = conn->query(ChunkType::ConfigNS,
                                                          differ.configDiffQuery());

            if (!cursor.get()) {
                // 'errMsg' was filled by the getChunkCursor() call.
                manager->_maxCollVersion = ChunkVersion();
                manager->_chunksMap.clear();
                connPtr->done();
                return false;
            }

            // Diff tracker should *always* find at least one chunk if this shard owns a chunk.
            int diffsApplied = differ.calculateConfigDiff(*cursor);
            if (diffsApplied > 0) {

                LOG(2) << "loaded " << diffsApplied
                       << " chunks into new chunk manager for " << ns
                       << " with version " << manager->_maxCollVersion << endl;

                manager->_maxShardVersion = versionMap[shard];
                manager->fillRanges();
                connPtr->done();
                return true;
            }
            else if(diffsApplied == 0) {

                warning() << "no chunks found when reloading " << ns
                          << ", previous version was "
                          << manager->_maxCollVersion.toString() << endl;

                manager->_maxCollVersion = ChunkVersion();
                manager->_chunksMap.clear();
                connPtr->done();
                return true;
            }
            else{

                // TODO: make this impossible by making sure we don't migrate / split on this
                // shard during the reload.  No chunks were found for the ns.

                *errMsg = str::stream() << "invalid chunks found when reloading " << ns
                                        << ", previous version was "
                                        << manager->_maxCollVersion.toString()
                                        << ", this should be rare";

                warning() << errMsg << endl;

                manager->_maxCollVersion = ChunkVersion();
                manager->_chunksMap.clear();
                connPtr->done();
                return false;
            }
        }
        catch (const DBException& e) {
            *errMsg = str::stream() << "caught exception accessing the config servers"
                                    << causedBy(e);

            // We deliberately do not return connPtr to the pool, since it was involved
            // with the error here.

            return false;
        }
    }

} // namespace mongo
