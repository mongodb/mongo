'use strict';

/**
 * Sets the internalQueryForceClassicEngine flag to true and false, and
 * asserts that find queries using the plan cache produce the correct results.
 *
 * @tags: [
 *     # Needed as the setParameter for ForceClassicEngine was introduced in 5.1.
 *     requires_fcv_51,
 *     # Our test infrastructure prevents tests which use the 'setParameter' command from running in
 *     # stepdown suites, since parameters are local to each mongod in the replica set.
 *     does_not_support_stepdowns,
 * ]
 */

var $config = (function() {
    let data = {originalParamValue: false};

    function getCollectionName(collName) {
        return "find_flip_sbe_enabled_" + collName;
    }

    function setup(db, collName, cluster) {
        const originalParamValue =
            db.adminCommand({getParameter: 1, internalQueryForceClassicEngine: 1});
        assertAlways.commandWorked(originalParamValue);
        assert(originalParamValue.hasOwnProperty("internalQueryForceClassicEngine"));
        this.originalParamValue = originalParamValue.internalQueryForceClassicEngine;
        const coll = db.getCollection(getCollectionName(collName));
        for (let i = 0; i < 10; ++i) {
            assertAlways.commandWorked(
                coll.insert({_id: i, x: i.toString(), y: i.toString(), z: i.toString()}));
        }

        assertAlways.commandWorked(coll.createIndex({x: 1}));
        assertAlways.commandWorked(coll.createIndex({y: 1}));
    }

    let states = (function() {
        function setForceClassicEngineOn(db, collName) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
        }

        function setForceClassicEngineOff(db, collName) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
        }

        function runQueriesAndCheckResults(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            for (let i = 0; i < 10; i++) {
                let res;
                try {
                    res = coll.find({x: i.toString(), y: i.toString(), z: i.toString()}).toArray();
                    assertAlways.eq(res.length, 1);
                    assertAlways.eq(res[0]._id, i);
                } catch (e) {
                    if (e.code !== ErrorCodes.QueryPlanKilled) {
                        throw e;  // This is an unexpected error, so we throw it again.
                    }
                }
            }
        }

        function createIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.createIndex({z: 1});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.IndexBuildAlreadyInProgress ||
                             res.code == ErrorCodes.IndexBuildAborted,
                         "Create index failed: " + tojson(res));
        }

        function dropIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.dropIndex({z: 1});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.IndexNotFound,
                         "Drop index failed: " + tojson(res));
        }

        return {
            setForceClassicEngineOn: setForceClassicEngineOn,
            setForceClassicEngineOff: setForceClassicEngineOff,
            runQueriesAndCheckResults: runQueriesAndCheckResults,
            createIndex: createIndex,
            dropIndex: dropIndex
        };
    })();

    let transitions = {
        setForceClassicEngineOn: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8
        },

        setForceClassicEngineOff: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8
        },

        runQueriesAndCheckResults: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        },

        createIndex: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.01,
            dropIndex: 0.01
        },

        dropIndex: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        }
    };

    function teardown(db, collName, cluster) {
        // Restore the original state of the ForceClassicEngine parameter.
        const setParam = this.originalParamValue;
        cluster.executeOnMongodNodes(function(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: setParam}));
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'setForceClassicEngineOn',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
