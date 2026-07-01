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
 *   requires_sharding,
 *   # This test just performs rename operations that can't be executed in transactions
 *   does_not_support_transactions,
 *  ]
 */

import {
    checkExceptionHasBeenThrown,
    getRandomCollName,
    getRandomDbName,
} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/random_ddl_utils.js";

const numDocs = 100;
const dbNames = ["db0", "db1"];
const collNames = [
    "rename_sharded_collectionA",
    "rename_sharded_collectionB",
    "rename_sharded_collectionC",
];

/*
 * Initialize a collection with expected number of chunks/documents and randomly distribute chunks
 */
function initAndFillShardedCollection(db, collName) {
    const coll = db[collName];
    const ns = coll.getFullName();
    db.adminCommand({shardCollection: ns, key: {x: 1}});

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());
}

/*
 * Keep track of raised exceptions in a collection to be checked during teardown.
 */
const expectedExceptions = [
    ErrorCodes.NamespaceNotFound,
    ErrorCodes.ConflictingOperationInProgress,
];
const logExceptionsDBName = "exceptions";
const logExceptionsCollName = "log";

function logException(db, exceptionCode) {
    db = db.getSiblingDB(logExceptionsDBName);
    const coll = db[logExceptionsCollName];
    assert.commandWorked(coll.insert({code: exceptionCode}));
}

export const $config = (function () {
    let states = {
        rename: function (db, collName, connCache) {
            const dbName = getRandomDbName(this.threadCount, dbNames);
            db = db.getSiblingDB(dbName);
            collName = getRandomCollName(this.threadCount, collNames);
            let srcColl = db[collName];
            const destCollName = getRandomCollName(this.threadCount, collNames);
            try {
                assert.commandWorked(srcColl.renameCollection(destCollName));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode == ErrorCodes.IllegalOperation) {
                    assert.eq(
                        collName,
                        destCollName,
                        "The FSM thread can fail with IllegalOperation just if a rename collection is happening on the same collection.",
                    );
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
        },
    };

    let setup = function (db, collName, cluster) {
        // Initialize databases
        for (let i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            const newDb = db.getSiblingDB(dbName);

            // Initialize one sharded collection per db
            initAndFillShardedCollection(newDb, collNames[Random.randInt(collNames.length)]);
        }
    };

    let teardown = function (db, collName, cluster) {
        // Ensure that NamespaceNotFound and ConflictingOperationInProgress have been raised at
        // least once: with a high level of concurrency, it's too improbable for such exceptions to
        // never be thrown (in that case, it's very likely that a bug has been introduced).
        expectedExceptions.forEach((errCode) =>
            checkExceptionHasBeenThrown(db, errCode, logExceptionsDBName, logExceptionsCollName),
        );

        // Check that at most one collection per test DB is present and that no data has been lost
        // upon multiple renames.
        for (let i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            db = db.getSiblingDB(dbName);
            const listColl = db.getCollectionNames();
            assert.eq(1, listColl.length);
            collName = listColl[0];
            const docCount = db[collName].countDocuments({});
            assert.eq(numDocs, docCount, "Unexpected number of chunks");
        }
    };

    let transitions = {rename: {rename: 1.0}};

    return {
        threadCount: 12,
        iterations: 64,
        startState: "rename",
        states: states,
        transitions: transitions,
        data: {},
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();
