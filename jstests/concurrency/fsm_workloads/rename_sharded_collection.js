'use strict';

/**
 * Perform continuous renames on 3 collections per database, with the objective to verify that:
 * - Upon successful renames, no data are lost
 * - Upon unsuccessful renames, no unexpected exception is thrown. Admitted errors:
 * ---- NamespaceNotFound (tried to rename a random non-existing collection)
 * ---- ConflictingOperationInProgress (tried to perform concurrent renames on the same source
 *      collection with different target collections)
 * - The aforementioned acceptable exceptions must be thrown at least once, given the high level of
 * concurrency
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_sharding,
 *   requires_fcv_50,
 *   # TODO (SERVER-56879): Support add/remove shards in new DDL paths
 *   does_not_support_add_remove_shards,
 *   # This test just performs rename operations that can't be executed in transactions
 *   does_not_support_transactions,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   featureFlagShardingFullDDLSupport
 *  ]
 */

const numChunks = 20;
const documentsPerChunk = 5;
const dbNames = ['db0', 'db1'];
const collNames = ['collA', 'collB', 'collC'];

/*
 * Initialize a collection with expected number of chunks/documents and randomly distribute chunks
 */
function initAndFillShardedCollection(db, collName, shardNames) {
    const coll = db[collName];
    const ns = coll.getFullName();
    db.adminCommand({shardCollection: ns, key: {x: 1}});

    var nextShardKeyValue = 0;
    for (var i = 0; i < numChunks; i++) {
        for (var j = 0; j < documentsPerChunk; j++) {
            coll.insert({x: nextShardKeyValue++});
        }

        assert.commandWorked(db.adminCommand({split: ns, middle: {x: nextShardKeyValue}}));

        const lastInsertedShardKeyValue = nextShardKeyValue - 1;

        // When balancer is enabled, move chunks could overlap and fail with
        // ConflictingOperationInProgress
        const res = db.adminCommand({
            moveChunk: ns,
            find: {x: lastInsertedShardKeyValue},
            to: shardNames[Random.randInt(shardNames.length)],
        });
        assert.commandWorkedOrFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
    }
}

/*
 * Get a random db/coll name from the test lists.
 *
 * Using the thread id to introduce more randomness: it has been observed that concurrent calls to
 * Random.randInt(array.length) are returning too often the same number to different threads.
 */
function getRandomDbName(tid) {
    return dbNames[Random.randInt(tid * tid) % dbNames.length];
}
function getRandomCollName(tid) {
    return collNames[Random.randInt(tid * tid) % collNames.length];
}

/*
 * Keep track of raised exceptions in a collection to be checked during teardown.
 */
const expectedExceptions =
    [ErrorCodes.NamespaceNotFound, ErrorCodes.ConflictingOperationInProgress];
const logExceptionsDBName = 'exceptions';
const logExceptionsCollName = 'log';

// TODO SERVER-56198: no need to log exceptions once the ticket will be completed.
function logException(db, exceptionCode) {
    db = db.getSiblingDB(logExceptionsDBName);
    const coll = db[logExceptionsCollName];
    assert.commandWorked(coll.insert({code: exceptionCode}));
}

function checkExceptionHasBeenThrown(db, exceptionCode) {
    db = db.getSiblingDB(logExceptionsDBName);
    const coll = db[logExceptionsCollName];
    const count = coll.countDocuments({code: exceptionCode});
    assert.gte(count, 1, 'No exception with error code ' + exceptionCode + ' has been thrown');
}

var $config = (function() {
    let states = {
        rename: function(db, collName, connCache) {
            const dbName = getRandomDbName(this.threadCount);
            db = db.getSiblingDB(dbName);
            collName = getRandomCollName(this.threadCount);
            var srcColl = db[collName];
            const destCollName = getRandomCollName(this.threadCount);
            try {
                assertAlways.commandWorked(srcColl.renameCollection(destCollName));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode == ErrorCodes.IllegalOperation) {
                    assert.eq(
                        collName,
                        destCollName,
                        "The FSM thread can fail with IllegalOperation just if a rename collection is happening on the same collection.");
                    return;
                }
                if (exceptionCode) {
                    logException(db, exceptionCode);
                    if (expectedExceptions.includes(exceptionCode)) {
                        return;
                    }
                }
                throw e;
            }
        }
    };

    let setup = function(db, collName, cluster) {
        const shardNames = Object.keys(cluster.getSerializedCluster().shards);
        const numShards = shardNames.length;

        // Initialize databases
        for (var i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName, primaryShard: shardNames[i % numShards]});
            // Initialize one sharded collection per db
            initAndFillShardedCollection(
                newDb, collNames[Random.randInt(collNames.length)], shardNames);
        }
    };

    let teardown = function(db, collName, cluster) {
        // TODO SERVER-56198: don't verify that exceptions have been thrown
        // Ensure that NamespaceNotFound and ConflictingOperationInProgress have been raised at
        // least once: with a high level of concurrency, it's too improbable for such exceptions to
        // never be thrown (in that case, it's very lickely that a bug has been introduced).
        expectedExceptions.forEach(errCode => checkExceptionHasBeenThrown(db, errCode));

        // Check that at most one collection per test DB is present and that no data has been lost
        // upon multiple renames.
        for (var i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            db = db.getSiblingDB(dbName);
            const listColl = db.getCollectionNames();
            assert.eq(1, listColl.length);
            collName = listColl[0];
            const numDocs = db[collName].countDocuments({});
            assert.eq(numChunks * documentsPerChunk, numDocs, 'Unexpected number of chunks');
        }
    };

    let transitions = {rename: {rename: 1.0}};

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'rename',
        states: states,
        transitions: transitions,
        data: {},
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
