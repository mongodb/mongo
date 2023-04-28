'use strict';

/**
 * Build indexes with different abort causes.
 * - Indexing error.
 * - Abort due to dropIndexes.
 * - Abort due to killOp on primary.
 *
 * Abort due to DiskSpaceMonitor is not tested as it would interfere with other concurrent tests
 * creating index builds. Similarly, killOp on secondary nodes is not tested as it can result in a
 * node crash, interfering with other tests.
 *
 * @tags: [
 *     creates_background_indexes,
 *     # The test uses $currentOp, which is not supported in transactions.
 *     does_not_support_transactions,
 *     requires_fcv_71,
 *     requires_replication
 * ]
 */

load("jstests/libs/fail_point_util.js");
load("jstests/noPassthrough/libs/index_build.js");

var $config = (function() {
    const data = {
        prefix: "index_build_abort_",
        nCollections: 3,
        nDocuments: 25000,
        expectedErrorCodes: [ErrorCodes.IndexBuildAborted, ErrorCodes.Interrupted, 13026],
        mutexColl: "index_build_abort_mutexes",
    };

    function randInt(max) {
        return Math.floor(Math.random() * max);
    }

    function getRandCollectionName() {
        return data.prefix + randInt(data.nCollections);
    }

    function getCollMutexName(collName) {
        return collName + "_mutex";
    }

    function mutexTryLock(db, collName) {
        const collMutex = getCollMutexName(collName);
        let doc = db[data.mutexColl].findAndModify(
            {query: {mutex: collMutex, locked: 0}, update: {$set: {locked: 1}}});
        if (doc === null) {
            return false;
        }
        return true;
    }

    function mutexUnlock(db, collName) {
        const collMutex = getCollMutexName(collName);
        db[data.mutexColl].update({mutex: collMutex}, {$set: {locked: 0}});
    }

    const states = {
        dropCollAndCreateIndexBuild: function dropCollAndCreateIndexBuild(db, collName) {
            const randomColl = getRandCollectionName();
            var coll = db[randomColl];
            if (mutexTryLock(db, randomColl)) {
                try {
                    // Having the collection drop outside the lock to allow a drop concurrent to an
                    // index build might be more interesting, but we'd also be allowing a drop in
                    // the middle of bulk insert, or before the createIndexes starts.
                    coll.drop();
                    var bulk = coll.initializeUnorderedBulkOp();
                    const failDocumentIndex = randInt(this.nDocuments);
                    for (var i = 0; i < this.nDocuments; ++i) {
                        if (failDocumentIndex == i) {
                            bulk.insert({a: [0, "a"]});
                        } else {
                            bulk.insert({a: [0, 0]});
                        }
                    }
                    let bulkRes = bulk.execute();
                    assertAlways.commandWorked(bulkRes);
                    assertAlways.eq(this.nDocuments, bulkRes.nInserted, tojson(bulkRes));
                    assertAlways.commandFailedWithCode(coll.createIndexes([{a: "2d"}]),
                                                       this.expectedErrorCodes);
                } finally {
                    mutexUnlock(db, randomColl);
                }
            }
        },
        dropIndexes: function dropIndexes(db, collName) {
            assertAlways.commandWorkedOrFailedWithCode(
                db.runCommand({dropIndexes: getRandCollectionName(), index: "*"}),
                ErrorCodes.NamespaceNotFound);
        },
        killOpIndexBuild: function killOpIndexBuild(db, collName) {
            let nTry = 0;
            while (nTry++ < 10) {
                try {
                    const opId = IndexBuildTest.getIndexBuildOpId(db, getRandCollectionName());
                    if (opId != -1) {
                        db.killOp(opId);
                        break;
                    }
                } catch (e) {
                    jsTestLog("Suppressed exception during killOp attempt: " + e);
                }
            }
        }
    };

    const transtitionToAllStates = {
        dropCollAndCreateIndexBuild: 1,
        dropIndexes: 1,
        killOpIndexBuild: 1,
    };
    const transitions = {
        dropCollAndCreateIndexBuild: transtitionToAllStates,
        dropIndexes: transtitionToAllStates,
        killOpIndexBuild: transtitionToAllStates
    };

    const setup = function(db, collName, cluster) {
        for (let coll = 0; coll < this.nCollections; ++coll) {
            const mutexName = getCollMutexName(data.prefix + coll);
            db[data.mutexColl].insert({mutex: mutexName, locked: 0});
        }
    };

    const teardown = function(db, collName, cluster) {
        for (let coll = 0; coll < this.nCollections; ++coll) {
            const collName = data.prefix + coll;
            db[collName].drop();
            db[getCollMutexName(collName)].drop();
        }
    };

    return {
        threadCount: 12,
        iterations: 200,
        startState: 'dropCollAndCreateIndexBuild',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
