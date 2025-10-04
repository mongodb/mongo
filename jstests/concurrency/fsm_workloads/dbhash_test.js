/**
 * Tests dbHash collisions in WT with full validation.
 * dbHash should not experience races on data, or EBUSY errors in the storage engine.
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 *   uses_full_validation,
 * ]
 */
export const dbPrefix = jsTestName() + "_db_";

export const $config = (function () {
    let states = {
        init: function (db, collName) {
            jsTestLog("init tid: " + this.tid);
        },
        dbHash: function (db, collName) {
            jsTestLog("dbHash: " + db + "." + collName + " tid: " + this.tid);
            let opTime = assert.commandWorked(
                db.runCommand({insert: collName, documents: [{x: 1}], writeConcern: {w: "majority"}}),
            ).operationTime;
            jsTestLog("dbHash opTime:" + tojson(opTime));
            jsTestLog("dbHash begin opTime:" + tojson(opTime));
            let dbHashRes = assert.commandWorked(
                db.collName.runCommand({
                    dbHash: 1,
                    readConcern: {level: "snapshot", atClusterTime: Timestamp(opTime["t"], opTime["i"])},
                }),
            );
            jsTestLog("dbHash done" + dbHashRes.timeMillis);
        },
        fullValidation: function (db, collName) {
            jsTestLog("fullValidation: " + db + "." + collName + " tid: " + this.tid);
            let res = assert.commandWorked(db.collName.validate({full: true}));
            jsTestLog("fullValidation done: " + db + "." + collName + " " + this.tid);
            assert(res.valid);
        },
    };

    const setSyncDelay = function (db, delay) {
        jsTestLog("setSyncDelay: ", delay);
        assert.commandWorked(db.adminCommand({setParameter: 1, syncdelay: delay}));
    };

    const setup = function (db, collName) {
        jsTestLog("Creating:" + db + "." + collName + " tid: " + this.tid);
        let x = "x".repeat(20 * 1024); // 20KB

        let bulk = db.collName.initializeOrderedBulkOp();
        for (let i = 0; i < 80; i++) {
            bulk.insert({_id: x + i.toString()});
        }
        assert.commandWorked(bulk.execute());

        // Avoid filling the cache by flushing on a shorter interval
        setSyncDelay(db, 10);

        jsTestLog("Creating done:" + db + "." + collName);
    };

    const teardown = function (db, collName) {
        setSyncDelay(db, 60);
    };

    const standardTransition = {
        dbHash: 0.5,
        fullValidation: 0.5,
    };

    const transitions = {
        init: standardTransition,
        dbHash: {dbHash: 0.8, fullValidation: 0.2},
        fullValidation: {dbHash: 0.2, fullValidation: 0.8},
    };

    return {
        threadCount: 5,
        iterations: 2,
        setup: setup,
        states: states,
        teardown: teardown,
        transitions: transitions,
    };
})();
