/**
 * rename_collection_cross_db_opposite.js
 *
 * Stresses lock ordering for renameCollectionAcrossDatabases by running two
 * threads that concurrently rename the same collection in opposite directions
 * between two databases (A->B and B->A).
 *
 * Success criterion: the server does not deadlock or crash.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   multiversion_incompatible,
 *   requires_replication,
 *   uses_rename,
 * ]
 */
export const $config = (function () {
    const threadCount = 2;
    const iterations = 5;
    const docCount = 100;
    const padSizeBytes = 16 * 1024;

    const data = {
        collName: jsTestName() + "_opposite",
        dbAName: jsTestName() + "_a",
        dbBName: jsTestName() + "_b",
    };

    const kAcceptableErrors = [
        ErrorCodes.MaxTimeMSExpired,
        ErrorCodes.LockTimeout,
        ErrorCodes.Interrupted,
        ErrorCodes.NamespaceNotFound,
        ErrorCodes.ConflictingOperationInProgress,
    ];

    function fillLargeCollection(db, collName) {
        assert.commandWorked(db.createCollection(collName));
        const bulk = db[collName].initializeUnorderedBulkOp();
        const pad = "x".repeat(padSizeBytes);
        for (let i = 0; i < docCount; ++i) {
            bulk.insert({_id: i, pad: pad});
        }
        assert.commandWorked(bulk.execute());
    }

    function doCrossDbRename(fromDB, toDB, collName) {
        if (!fromDB[collName].exists()) {
            return;
        }

        const res = fromDB.adminCommand({
            renameCollection: fromDB.getName() + "." + collName,
            to: toDB.getName() + "." + collName,
            dropTarget: true,
            maxTimeMS: 5 * 60 * 1000,
        });

        if (!res.ok) {
            assert(kAcceptableErrors.includes(res.code), () => tojson(res));
        }
    }

    const states = {
        loop: function (db, collName) {
            const dbA = db.getSiblingDB(this.dbAName);
            const dbB = db.getSiblingDB(this.dbBName);
            if (this.tid === 0) {
                doCrossDbRename(dbA, dbB, this.collName);
            } else {
                doCrossDbRename(dbB, dbA, this.collName);
            }
        },
    };

    function setup(db, collName, cluster) {
        const dbA = db.getSiblingDB(data.dbAName);
        const dbB = db.getSiblingDB(data.dbBName);
        fillLargeCollection(dbA, data.collName);
        fillLargeCollection(dbB, data.collName);
    }

    return {
        threadCount,
        iterations,
        data,
        states,
        startState: "loop",
        transitions: {loop: {loop: 1}},
        setup,
    };
})();
