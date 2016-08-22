/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <set>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

class ChunkManager;
class CollectionType;
class DatabaseType;
class DBConfig;
class OperationContext;

struct CollectionInfo {
    CollectionInfo() {
        _dirty = false;
        _dropped = false;
    }

    CollectionInfo(OperationContext* txn, const CollectionType& in, repl::OpTime);
    ~CollectionInfo();

    bool isSharded() const {
        return _cm.get();
    }

    std::shared_ptr<ChunkManager> getCM() const {
        return _cm;
    }

    void resetCM(ChunkManager* cm);

    void unshard();

    bool isDirty() const {
        return _dirty;
    }

    bool wasDropped() const {
        return _dropped;
    }

    void save(OperationContext* txn, const std::string& ns);

    void useChunkManager(std::shared_ptr<ChunkManager> manager);

    bool unique() const {
        return _unique;
    }

    BSONObj key() const {
        return _key;
    }

    repl::OpTime getConfigOpTime() const {
        return _configOpTime;
    }

private:
    BSONObj _key;
    bool _unique;
    std::shared_ptr<ChunkManager> _cm;
    bool _dirty;
    bool _dropped;
    repl::OpTime _configOpTime;
};
/**
*The two new structs can store the datas of the BSONObj we get from
*The GeoMetaDataNS and IndexMetaDataNS.
*Store the information so we don't have to connect the database everytime
*/
struct GeoMetaData{
    std::string datanamespace;
    std::string column_name;
    int index_type;
    OID index_info;
    int gtype;
    int srid;
    int crs_type;
    double tolerrance;

    BSONObj toBson()
    {
        BSONObjBuilder bdr;
        bdr.append("NAMESPACE", datanamespace);
        bdr.append("COLUMN_NAME", column_name);
        bdr.append("INDEX_TYPE", index_type);
        bdr.append("INDEX_INFO", index_info);
        bdr.append("SDO_GTYPE", gtype);
        bdr.append("SRID", srid);
        bdr.append("CRS_TYPE", srid);
        bdr.append("TOLERANCE", tolerrance);
        return bdr.obj();
    }
};

struct RtreeMetaData{
    int maxnode;
    int maxleaf;
    OID root_key;
};
/**
 * top level configuration for a database
 */
class DBConfig {
public:
    DBConfig(std::string name, const DatabaseType& dbt, repl::OpTime configOpTime);
    ~DBConfig();

    /**
     * The name of the database which this entry caches.
     */
    const std::string& name() const {
        return _name;
    }

    /**
     * Whether sharding is enabled for this database.
     */
    bool isShardingEnabled() const {
        return _shardingEnabled;
    }

    const ShardId& getPrimaryId() const {
        return _primaryId;
    }

    /**
     * Removes all cached metadata for the specified namespace so that subsequent attempts to
     * retrieve it will cause a full reload.
     */
    void invalidateNs(const std::string& ns);

    void enableSharding(OperationContext* txn);

    /**
       @return true if there was sharding info to remove
     */
    bool removeSharding(OperationContext* txn, const std::string& ns);

    /**
     * @return whether or not the 'ns' collection is partitioned
     */
    bool isSharded(const std::string& ns);

    // Atomically returns *either* the chunk manager *or* the primary shard for the collection,
    // neither if the collection doesn't exist.
    void getChunkManagerOrPrimary(OperationContext* txn,
                                  const std::string& ns,
                                  std::shared_ptr<ChunkManager>& manager,
                                  std::shared_ptr<Shard>& primary);

    std::shared_ptr<ChunkManager> getChunkManager(OperationContext* txn,
                                                  const std::string& ns,
                                                  bool reload = false,
                                                  bool forceReload = false);
    std::shared_ptr<ChunkManager> getChunkManagerIfExists(OperationContext* txn,
                                                          const std::string& ns,
                                                          bool reload = false,
                                                          bool forceReload = false);

    /**
     * Returns shard id for primary shard for the database for which this DBConfig represents.
     */
    const ShardId& getShardId(OperationContext* txn, const std::string& ns);

    void setPrimary(OperationContext* txn, const ShardId& newPrimaryId);

    /**
     * Returns true if it is successful at loading the DBConfig, false if the database is not found,
     * and throws on all other errors.
     */
    bool load(OperationContext* txn);
    bool reload(OperationContext* txn);

	/**
	*metadata oprations.
	*/
	//insert geometry metadata
	void registerGeometry(OperationContext* txn,BSONObj bdr);
	//get geometry metadata
	BSONObj getGeometry(OperationContext* txn,BSONObj query);
	//update geometry metadata
	void  updateGeometry(OperationContext* txn,BSONObj query, BSONObj obj);
	//delete geometry metadata
	void deleteGeometry(OperationContext* txn,BSONObj query);
	//check whether geometry metadata related to given field exist
	bool checkGeoExist(OperationContext* txn,BSONObj bdr);
	//check whether R-tree related to given field exist
	bool checkRtreeExist(OperationContext* txn,BSONObj bdr);
	//insert index metadata
	void insertIndexMetadata(OperationContext* txn,BSONObj bdr);
	//get index metadata
	BSONObj getIndexMetadata(OperationContext* txn,BSONObj query);
	//update index metadata
	void updateIndexMetadata(OperationContext* txn, BSONObj query,BSONObj obj);
	//delete index metadata
	void deleteIndexMetadata(OperationContext* txn,BSONObj query);
	
    bool dropDatabase(OperationContext*, std::string& errmsg);

    void getAllShardIds(std::set<ShardId>* shardIds);
    void getAllShardedCollections(std::set<std::string>& namespaces);

protected:
    typedef std::map<std::string, CollectionInfo> CollectionInfoMap;

    bool _dropShardedCollections(OperationContext* txn,
                                 int& num,
                                 std::set<ShardId>& shardIds,
                                 std::string& errmsg);

    /**
     * Returns true if it is successful at loading the DBConfig, false if the database is not found,
     * and throws on all other errors.
     */
    bool _load(OperationContext* txn);

    void _save(OperationContext* txn, bool db = true, bool coll = true);

    // Name of the database which this entry caches
    const std::string _name;

    // Primary shard id
    ShardId _primaryId;

    // Whether sharding has been enabled for this database
    bool _shardingEnabled;

    // Set of collections and lock to protect access
    stdx::mutex _lock;
    CollectionInfoMap _collections;

    GeoMetaData _currGeoMeta;
	RtreeMetaData _currIndexMeta;

    // OpTime of config server when the database definition was loaded.
    repl::OpTime _configOpTime;

    // Ensures that only one thread at a time loads collection configuration data from
    // the config server
    stdx::mutex _hitConfigServerLock;
};


class ConfigServer {
public:
    static void reloadSettings(OperationContext* txn);

    /**
     * For use in mongos and mongod which needs notifications about changes to shard and config
     * server replset membership to update the ShardRegistry.
     *
     * This is expected to be run in an existing thread.
     */
    static void replicaSetChangeShardRegistryUpdateHook(const std::string& setName,
                                                        const std::string& newConnectionString);

    /**
     * For use in mongos which needs notifications about changes to shard replset membership to
     * update the config.shards collection.
     *
     * This is expected to be run in a brand new thread.
     */
    static void replicaSetChangeConfigServerUpdateHook(const std::string& setName,
                                                       const std::string& newConnectionString);
};

}  // namespace mongo
