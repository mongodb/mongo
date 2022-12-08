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

// The test is overriding some cluster parameters (which original values will be overridden at
// teardown time)
let originalChunkDefragmentationThrottlingMS;
let originalChunkSizeMB;
let balancerEnabledInSuite;

const maxChunkSizeMB = 1;

function createShardedCollection(db, collName) {
    let newColl = db.getName() + "." + collName;
    jsTest.log("Creating collection " + newColl);
    assertAlways.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    assertAlways.commandWorked(db.adminCommand({shardCollection: newColl, key: {_id: 1}}));
}

function insertIntoCollection(db, collName) {
    jsTest.log("Inserting documents into collection " + collName);
    let coll = db[collName];
    let randomKey = Random.randInt(Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
    const bigString = "X".repeat(256 * 1024);
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 10; i++) {
        bulk.insert({_id: randomKey + i, bigString: bigString});
    }
    assertAlways.commandWorked(bulk.execute());
    jsTest.log("Inserts completed on collection " + collName + " starting from key: " + randomKey);
}

var $config = (function() {
    var states = {
        init: function init(db, collName, connCache) {
            this.prefix = "autosplit_setFCV_downgrade_from_60";
            this.num = 0;
        },

        createAndInsert: function createAndInsert(db, collName, connCache) {
            let newColl = db.getName() + "." + this.prefix + "_" + this.num++;
            try {
                createShardedCollection(db, newColl);
            } catch (e) {
                if (e.code != ErrorCodes.NamespaceNotFound &&
                    e.code != ErrorCodes.NamespaceNotSharded &&
                    e.code != ErrorCodes.IllegalOperation &&
                    e.code != ErrorCodes.ConflictingOperationInProgress) {
                    throw e;
                }
                jsTest.log("Create collection failed with error: " + tojson(e));
                return;
            }
            jsTest.log("Created collection " + newColl);
            insertIntoCollection(db, newColl);
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
            insertIntoCollection(db, randomColl);
        },

        setFCV: function setFCV(db, collName, connCache) {
            const performDowngrade = Random.randInt(2) === 1;
            const targetFCV = performDowngrade ? lastLTSFCV : latestFCV;
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
        init: {createAndInsert: 1},
        createAndInsert: {drop: 0.165, insert: 0.33, setFCV: 0.495},
        drop: {createAndInsert: 0.33, insert: 0.33, setFCV: 0.33},
        insert: {createAndInsert: 0.495, drop: 0.165, setFCV: 0.33},
        setFCV: {createAndInsert: 0.33, drop: 0.165, insert: 0.495}
    };
    function setup(db, collName, cluster) {
        balancerEnabledInSuite = sh.getBalancerState();
        const configDB = cluster.getDB('config');
        originalChunkSizeMB = configDB.settings.findOne({_id: "chunkSize"});
        assert.commandWorked(db.adminCommand({balancerStop: 1}));
        configDB.settings.save({_id: "chunksize", value: maxChunkSizeMB});
        cluster.executeOnConfigNodes((db) => {
            const res = db.adminCommand({setParameter: 1, chunkDefragmentationThrottlingMS: 0});
            assert.commandWorked(res);
            originalChunkDefragmentationThrottlingMS = res.was;
        });
    }

    function teardown(db, collName, cluster) {
        const configDB = cluster.getDB('config');
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        if (originalChunkSizeMB) {
            configDB.settings.save({_id: "chunksize", value: originalChunkSizeMB.value});
        } else {
            configDB.settings.deleteOne({_id: "chunksize"});
        }
        cluster.executeOnConfigNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                chunkDefragmentationThrottlingMS: originalChunkDefragmentationThrottlingMS
            }));
        });

        if (balancerEnabledInSuite) {
            assert.commandWorked(db.adminCommand({balancerStart: 1}));
        }
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
