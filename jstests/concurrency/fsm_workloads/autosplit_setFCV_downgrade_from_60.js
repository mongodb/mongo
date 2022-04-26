'use strict';

/**
 * autosplit_setFCV_downgrade_from_60.js
 *
 * TODO (SERVER-65332) remove this test before 6.1 release.
 *
 * Checks that chunks are split when downgrading to an FCV lower than 6.0.
 *
 * @tags: [requires_sharding,
 *  # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible]
 */

load('jstests/sharding/libs/find_chunks_util.js');
load('jstests/sharding/libs/defragmentation_util.js');
load('jstests/concurrency/fsm_workload_helpers/chunks.js');

function getRandomCollectionName(configDB, prefix) {
    const collectionCursor = configDB.collections.aggregate(
        [{$match: {_id: {$regex: ".*" + prefix + ".*"}}}, {$sample: {size: 1}}]);
    if (!collectionCursor.hasNext()) {
        return undefined;
    } else {
        let fullName = collectionCursor.next()._id;
        return fullName.substring(fullName.indexOf(".") + 1);
    }
}

var $config = (function() {
    var states = {
        init: function init(db, collName, connCache) {
            this.prefix = "autosplit_setFCV_downgrade_from_60";
            this.num = 0;
        },

        create: function create(db, collName, connCache) {
            let newColl = db.getName() + "." + this.prefix + "_" + this.num++;
            jsTest.log("Creating collection " + newColl);
            try {
                assertAlways.commandWorked(db.adminCommand({enableSharding: db.getName()}));
                assertAlways.commandWorked(
                    db.adminCommand({shardCollection: newColl, key: {_id: 1}}));
                assertAlways.commandWorked(db.adminCommand(
                    {configureCollectionBalancing: newColl, enableAutoSplitter: false}));
            } catch (e) {
                if (e.code != ErrorCodes.NamespaceNotFound &&
                    e.code != ErrorCodes.IllegalOperation &&
                    e.code != ErrorCodes.ConflictingOperationInProgress) {
                    throw e;
                }
                jsTest.log("Create collection failed with error: " + tojson(e));
                return;
            }
            jsTest.log("Created collection " + newColl);
        },

        drop: function drop(db, collName, connCache) {
            let randomColl = getRandomCollectionName(db.getSiblingDB("config"), this.prefix);
            if (randomColl === undefined) {
                return;
            }
            jsTest.log("Dropping collection " + randomColl);
            assertAlways(db[randomColl].drop());
            jsTest.log("Dropped collection " + randomColl);
        },

        insert: function insert(db, collName, connCache) {
            let randomColl = getRandomCollectionName(db.getSiblingDB("config"), this.prefix);
            if (randomColl === undefined)
                return;
            jsTest.log("Inserting documents into collection " + randomColl);
            let coll = db[randomColl];
            let randomKey = Random.randInt(Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
            const bigString = "X".repeat(256 * 1024);
            let bulk = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < 10; i++) {
                bulk.insert({_id: randomKey + i, bigString: bigString});
            }
            assertAlways.commandWorked(bulk.execute());
            jsTest.log("Inserts completed on collection " + randomColl +
                       " starting from key: " + randomKey);
        },

        setFCV: function setFCV(db, collName, connCache) {
            let randomFCVVal = Random.randInt(2);
            let targetFCV = randomFCVVal === 1 ? lastContinuousFCV : latestFCV;
            jsTest.log("Setting FCV to " + targetFCV);
            try {
                assertAlways.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
            } catch (e) {
                if (e.code === 5147403) {
                    jsTest.log("Invalid FCV transition");
                    return;
                }
                throw e;
            }
            jsTestLog("Finished setting FCV to " + targetFCV);
        },
    };

    var transitions = {
        init: {create: 0.5, setFCV: 0.5},
        create: {drop: 0.165, insert: 0.495, setFCV: 0.33},
        drop: {create: 0.33, insert: 0.33, setFCV: 0.33},
        insert: {create: 0.495, drop: 0.165, setFCV: 0.33},
        setFCV: {create: 0.33, drop: 0.165, insert: 0.495}
    };

    function setup(db, collName, cluster) {
        let configDB = cluster.getDB('config');
        configDB.settings.save({_id: "chunksize", value: /*<sizeInMB>*/ 1});
    }

    function teardown(db, collName, cluster) {
    }

    return {
        threadCount: 5,
        iterations: 10,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
