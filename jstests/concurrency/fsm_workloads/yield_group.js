'use strict';

/**
 * Tests that the group command either succeeds or fails gracefully when interspersed with inserts
 * on a capped collection. Designed to reproduce SERVER-34725.
 */
var $config = (function() {

    var states = {
        /*
         * Issue a group command against the capped collection.
         */
        group: function group(db, collName) {
            try {
                assert.commandWorked(db.runCommand(
                    {group: {ns: collName, key: {_id: 1}, $reduce: function() {}, initial: {}}}));
            } catch (ex) {
                assert.eq(ErrorCodes.CappedPositionLost, ex.code);
            }
        },

        /**
         * Inserts a document into the capped collection.
         */
        insert: function insert(db, collName) {
            assertAlways.writeOK(db[collName].insert({a: 1}));
        }
    };

    var transitions = {
        insert: {insert: 0.5, group: 0.5},
        group: {insert: 0.5, group: 0.5},
    };

    function setup(db, collName, cluster) {
        const nDocs = 200;

        // Create the test capped collection, with a max number of documents.
        db[collName].drop();
        assert.commandWorked(db.createCollection(collName, {
            capped: true,
            size: 4096,
            max: this.nDocs,  // Set the maximum number of documents in the capped collection such
                              // that additional inserts will drop older documents and increase the
                              // likelihood of losing the capped position.
        }));

        // Lower the following parameters to increase the probability of yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 5}));
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));
        });

        // Set up some data to query.
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs; i++) {
            bulk.insert({_id: i});
        }
        assertAlways.writeOK(bulk.execute());
    }

    /*
     * Reset parameters.
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
        threadCount: 5,
        iterations: 50,
        startState: 'insert',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: {}
    };
})();
