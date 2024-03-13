/**
 * Helper functions that help get information or manipulate nodes in the fixture, whether it's a
 * replica set, a sharded cluster, etc.
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export var FixtureHelpers = (function() {
    function _getHostStringForReplSet(connectionToNodeInSet) {
        const isMaster = assert.commandWorked(connectionToNodeInSet.getDB("test").isMaster());
        assert(
            isMaster.hasOwnProperty("setName"),
            "Attempted to get replica set connection to a node that is not part of a replica set");
        return isMaster.setName + "/" + isMaster.hosts.join(",");
    }

    /**
     * Returns an array of connections to each data-bearing replica set in the fixture (not
     * including the config servers).
     */
    function getAllReplicas(db) {
        let replicas = [];
        if (isMongos(db)) {
            const shardObjs = db.getSiblingDB("config").shards.find().sort({_id: 1});
            replicas = shardObjs.map((shardObj) => new ReplSetTest(shardObj.host));
        } else {
            replicas = [new ReplSetTest(_getHostStringForReplSet(db.getMongo()))];
        }
        return replicas;
    }

    /**
     * Uses ReplSetTest.awaitReplication() on each replica set in the fixture to wait for each node
     * in each replica set in the fixture (besides the config servers) to reach the same op time.
     * Asserts if the fixture is a standalone or if the shards are standalones.
     */
    function awaitReplication(db) {
        getAllReplicas(db).forEach((replSet) => replSet.awaitReplication());
    }

    /**
     * Uses ReplSetTest.awaitLastOpCommitted() on each replica set in the fixture (besides the
     * config servers) to wait for the last oplog entry on the respective primary to be visible in
     * the committed snapshot view of the oplog on all secondaries.
     *
     * Asserts if the fixture is a standalone or if the shards are standalones.
     */
    function awaitLastOpCommitted(db) {
        getAllReplicas(db).forEach((replSet) => replSet.awaitLastOpCommitted());
    }

    /**
     * Looks for an entry in the config database for the given collection, to check whether it's
     * sharded.
     */
    function isSharded(coll) {
        const collEntry =
            coll.getDB().getSiblingDB("config").collections.findOne({_id: coll.getFullName()});
        if (collEntry === null) {
            return false;
        }
        return collEntry.unsplittable === null || !collEntry.unsplittable;
    }

    /**
     * Looks for an entry in the sharding catalog for the given collection, to check whether it's
     * unsplittable.
     */
    function isUnsplittable(coll) {
        const collEntry =
            coll.getDB().getSiblingDB("config").collections.findOne({_id: coll.getFullName()});
        if (collEntry === null) {
            return false;
        }
        return collEntry.unsplittable !== null && collEntry.unsplittable;
    }

    /**
     * Looks for an entry in the sharding catalog for the given collection to check whether it is
     * present.
     *
     * TODO (SERVER-86443): remove this utility once all collections are tracked.
     */
    function isTracked(coll) {
        return isSharded(coll) || isUnsplittable(coll);
    }

    /**
     * Returns an array with the shardIds that own data for the given collection.
     */
    function getShardsOwningDataForCollection(coll) {
        if (isSharded(coll) || isUnsplittable(coll)) {
            const res = db.getSiblingDB('config')
                .collections
                .aggregate([
                    {$match: {_id: coll.getFullName()}},
                    {
                        $lookup:
                            {from: 'chunks', localField: 'uuid', foreignField: 'uuid', as: 'chunks'}
                    },
                    {$group: {_id: '$chunks.shard'}}
                ])
                .toArray();
            return res.map((x) => x._id);
        } else {
            const dbMetadata =
                db.getSiblingDB('config').databases.findOne({_id: coll.getDB().getName()});
            return dbMetadata ? [dbMetadata.primary] : [];
        }
    }

    /**
     * Utility to determine whether the collections in 'collList' are colocated or not.
     */
    function areCollectionsColocated(collList) {
        if (!FixtureHelpers.isMongos(db)) {
            return true;
        }
        let set = new Set();
        for (const coll of collList) {
            for (const shard of getShardsOwningDataForCollection(coll)) {
                set.add(shard);
                if (set.size > 1) {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * Returns the resolved view definition for 'collName' if it is a view, 'undefined' otherwise.
     */
    function getViewDefinition(db, collName) {
        return db.getCollectionInfos({type: "view", name: collName}).shift();
    }

    /**
     * Returns the number of shards that 'coll' has any chunks on. Returns 1 if the collection is
     * not sharded. Note that if the balancer is enabled then the number of shards with chunks for
     * this collection can change at any moment.
     */
    function numberOfShardsForCollection(coll) {
        if (!isMongos(coll.getDB()) || !isSharded(coll)) {
            // If we're not talking to a mongos, or the collection is not sharded, there is one
            // shard.
            return 1;
        }
        const collMetadata =
            db.getSiblingDB("config").collections.findOne({_id: coll.getFullName()});
        if (collMetadata.timestamp) {
            return db.getSiblingDB("config")
                .chunks.distinct("shard", {uuid: collMetadata.uuid})
                .length;
        } else {
            return db.getSiblingDB("config")
                .chunks.distinct("shard", {ns: coll.getFullName()})
                .length;
        }
    }

    /**
     * Runs the function given by 'func' passing the database given by 'db' from each shard nodes in
     * the fixture (besides the config servers). Returns the array of return values from executed
     * functions. If the fixture is a standalone, will run the function on the database directly.
     */
    function mapOnEachShardNode({db, func, primaryNodeOnly}) {
        function getRequestedConns(host) {
            const conn = new Mongo(host);
            const isMaster = conn.getDB("test").isMaster();

            if (isMaster.hasOwnProperty("setName")) {
                // It's a repl set.
                const rs = new ReplSetTest(host);
                return primaryNodeOnly ? [rs.getPrimary()] : rs.nodes;
            } else {
                // It's a standalone.
                return [conn];
            }
        }

        let connList = [];
        if (isMongos(db)) {
            const shardObjs = db.getSiblingDB("config").shards.find().sort({_id: 1}).toArray();

            for (let shardObj of shardObjs) {
                connList = connList.concat(getRequestedConns(shardObj.host));
            }
        } else {
            connList = connList.concat(getRequestedConns(db.getMongo().host));
        }

        return connList.map((conn) => func(conn.getDB(db.getName())));
    }

    /**
     * Runs the command given by 'cmdObj' on the database given by 'db' on each shard nodes in
     * the fixture (besides the config servers). Asserts that each command works, and returns an
     * array with the responses from each shard, or with a single element if the fixture was a
     * replica set. If the fixture is a standalone, will run the command directly.
     */
    function runCommandOnAllShards({db, cmdObj, primaryNodeOnly}) {
        return mapOnEachShardNode({
            db,
            func: (primaryDb) => assert.commandWorked(primaryDb.runCommand(cmdObj)),
            primaryNodeOnly
        });
    }

    /**
     * A helper function for 'runCommandOnAllShards' to only run command on the primary nodes.
     */
    function runCommandOnEachPrimary({db, cmdObj}) {
        return runCommandOnAllShards({db, cmdObj, primaryNodeOnly: true});
    }

    /**
     * Returns a connection to the replica set primary for the primary shard for the given database.
     * Returns the same connection that 'db' is using if the fixture is not a sharded cluster.
     */
    function getPrimaryForNodeHostingDatabase(db) {
        if (!isMongos(db)) {
            return db.getMongo();
        }
        const configDB = db.getSiblingDB("config");
        let shardConn = null;
        configDB.databases.find().forEach(function(dbObj) {
            if (dbObj._id === db.getName()) {
                const shardObj = configDB.shards.findOne({_id: dbObj.primary});
                shardConn = new Mongo(shardObj.host);
            }
        });
        assert.neq(null, shardConn, "could not find shard hosting database " + db.getName());
        return shardConn;
    }

    /**
     * Returns a collection of connections to each primary in a cluster.
     */
    function getPrimaries(db) {
        return getAllReplicas(db).map((replSet) => replSet.getPrimary());
    }

    /**
     * Returns a collection of connections to secondaries in a cluster.
     */
    function getSecondaries(db) {
        return getAllReplicas(db).reduce((array, replSet) => {
            return array.concat(replSet.getSecondaries());
        }, []);
    }

    /**
     * Returns true if we have a replica set.
     */
    function isReplSet(db) {
        const primaryInfo = db.isMaster();
        return primaryInfo.hasOwnProperty('setName');
    }

    /**
     * Returns true if we have a standalone mongod.
     */
    function isStandalone(db) {
        return !isMongos(db) && !isReplSet(db);
    }

    return {
        isMongos: isMongos,
        isSharded: isSharded,
        isUnsplittable: isUnsplittable,
        isTracked: isTracked,
        areCollectionsColocated: areCollectionsColocated,
        getShardsOwningDataForCollection: getShardsOwningDataForCollection,
        getViewDefinition: getViewDefinition,
        numberOfShardsForCollection: numberOfShardsForCollection,
        awaitReplication: awaitReplication,
        awaitLastOpCommitted: awaitLastOpCommitted,
        mapOnEachShardNode: mapOnEachShardNode,
        runCommandOnAllShards: runCommandOnAllShards,
        runCommandOnEachPrimary: runCommandOnEachPrimary,
        getAllReplicas: getAllReplicas,
        getPrimaries: getPrimaries,
        getSecondaries: getSecondaries,
        getPrimaryForNodeHostingDatabase: getPrimaryForNodeHostingDatabase,
        isReplSet: isReplSet,
        isStandalone: isStandalone,
    };
})();
