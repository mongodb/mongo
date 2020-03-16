"use strict";

/**
 * Executes query operations that can yield while the source collection is dropped and recreated.
 */

var $config = (function() {
    const data = {
        kAllowedErrors: [
            ErrorCodes.ConflictingOperationInProgress,
            ErrorCodes.CursorNotFound,
            ErrorCodes.DuplicateKey,
            ErrorCodes.IndexBuildAborted,
            ErrorCodes.NoProgressMade,
            ErrorCodes.OperationFailed,
            ErrorCodes.QueryPlanKilled,
        ],
        nDocs: 200,
        genUpdateDoc: function genUpdateDoc() {
            const newVal = Random.randInt(this.nDocs);
            return {$set: {a: newVal}};
        },
        // TODO SERVER-44673: Replace this function with calls to
        // commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors).
        assertWriteWorkedOrFailedWithExpectedCode:
            function assertWriteWorkedOrFailedWithExpectedCode(cmdRes) {
                if (cmdRes.ok) {
                    if (cmdRes.hasOwnProperty("writeErrors") && cmdRes.writeErrors.length > 0) {
                        assertAlways(this.kAllowedErrors.includes(cmdRes.writeErrors[0].code),
                                     cmdRes);
                    }

                    return;
                }

                assertAlways.commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors);
            },
        create: function create(db, collName) {
            for (let i = 0; i < this.nDocs; i++) {
                const cmdRes = db.runCommand({
                    update: collName,
                    updates: [{
                        q: {_id: i},
                        u: {$set: {a: i, b: this.nDocs - i, c: i, d: this.nDocs - i, e: "foo"}},
                        upsert: true
                    }]
                });
                this.assertWriteWorkedOrFailedWithExpectedCode(cmdRes);
            }
            assertAlways.commandWorkedOrFailedWithCode(db[collName].createIndex({a: 1}),
                                                       this.kAllowedErrors);
        }
    };

    var states = {
        query: function query(db, collName) {
            let cmdRes = db.runCommand(
                {find: collName, filter: {c: {$lt: this.nDocs}}, batchSize: this.nDocs});
            assertAlways.commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors);

            if (cmdRes.hasOwnProperty("cursor") && cmdRes.cursor.id > 0) {
                cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
                assertAlways.commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors);
            }
        },

        update: function update(db, collName) {
            const cmdRes = db.runCommand({
                update: collName,
                updates: [{q: {_id: Random.randInt(this.nDocs)}, u: this.genUpdateDoc()}]
            });

            this.assertWriteWorkedOrFailedWithExpectedCode(cmdRes);
        },

        remove: function remove(db, collName) {
            const cmdRes = db.runCommand(
                {delete: collName, deletes: [{q: {_id: Random.randInt(this.nDocs)}, limit: 1}]});

            this.assertWriteWorkedOrFailedWithExpectedCode(cmdRes);
        },

        count: function count(db, collName) {
            const cmdRes = db.runCommand({count: collName, query: {a: {$lt: this.nDocs}}});
            assertAlways.commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors);
        },

        distinct: function distinct(db, collName) {
            const cmdRes =
                db.runCommand({distinct: collName, key: "a", query: {a: {$lt: this.nDocs}}});
            assertAlways.commandWorkedOrFailedWithCode(cmdRes, this.kAllowedErrors);
        },

        recreateColl: function recreateColl(db, collName) {
            const cmdRes = db[collName].drop();
            this.create(db, collName);
        },
    };

    const kAllStatesEqual =
        {update: 0.18, remove: 0.18, query: 0.18, count: 0.18, distinct: 0.18, recreateColl: 0.1};
    const transitions = {
        update: kAllStatesEqual,
        remove: kAllStatesEqual,
        query: kAllStatesEqual,
        count: kAllStatesEqual,
        distinct: kAllStatesEqual,
        recreateColl: kAllStatesEqual,
    };

    /**
     * Lowers yielding parameters to increase frequency and sets up collection.
     */
    function setup(db, collName, cluster) {
        // Lower the following parameters to force even more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 5}));
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));
        });

        data.create(db, collName);
    }

    /**
     * Disables failpoints.
     */
    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function resetYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 128}));
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 10}));
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'update',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
