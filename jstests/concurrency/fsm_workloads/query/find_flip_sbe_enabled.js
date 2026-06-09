/**
 * Sets the internalQueryFrameworkControl flag to "forceClassicEngine" and "trySbeEngine", and
 * asserts that find queries using the plan cache produce the correct results.
 *
 * @tags: [
 *     # Our test infrastructure prevents tests which use the 'setParameter' command from running in
 *     # stepdown suites, since parameters are local to each mongod in the replica set.
 *     does_not_support_stepdowns,
 *     # The balancer may issue moveCollection requests, which cause concurrent index builds to be
 *     # aborted.
 *     assumes_balancer_off,
 * ]
 */
import {setParameterOnAllNodes} from "jstests/concurrency/fsm_workload_helpers/set_parameter.js";

export const $config = (function () {
    let data = {};
    let originalFrameworkControl;

    function getCollectionName(collName) {
        return "find_flip_sbe_enabled_" + collName;
    }

    function setup(db, collName, cluster) {
        originalFrameworkControl = setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryFrameworkControl",
            newValue: "trySbeEngine",
            onMongos: false,
        });

        const coll = db.getCollection(getCollectionName(collName));
        for (let i = 0; i < 10; ++i) {
            assert.commandWorked(coll.insert({_id: i, x: i.toString(), y: i.toString(), z: i.toString()}));
        }

        assert.commandWorked(coll.createIndex({x: 1}));
        assert.commandWorked(coll.createIndex({y: 1}));
    }

    let states = (function () {
        function setForceClassicEngineOn(db, collName) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
            );
        }

        function setForceClassicEngineOff(db, collName) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
        }

        function runQueriesAndCheckResults(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            for (let i = 0; i < 10; i++) {
                let res;
                try {
                    res = coll.find({x: i.toString(), y: i.toString(), z: i.toString()}).toArray();
                    assert.eq(res.length, 1);
                    assert.eq(res[0]._id, i);
                } catch (e) {
                    if (e.code !== ErrorCodes.QueryPlanKilled) {
                        throw e; // This is an unexpected error, so we throw it again.
                    }
                }
            }
        }

        function createIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.createIndex({z: 1});
            assert(
                res.ok === 1 ||
                    res.code === ErrorCodes.IndexBuildAlreadyInProgress ||
                    res.code == ErrorCodes.IndexBuildAborted,
                "Create index failed: " + tojson(res),
            );
        }

        function dropIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.dropIndex({z: 1});
            assert(res.ok === 1 || res.code === ErrorCodes.IndexNotFound, "Drop index failed: " + tojson(res));
        }

        return {
            setForceClassicEngineOn: setForceClassicEngineOn,
            setForceClassicEngineOff: setForceClassicEngineOff,
            runQueriesAndCheckResults: runQueriesAndCheckResults,
            createIndex: createIndex,
            dropIndex: dropIndex,
        };
    })();

    let transitions = {
        setForceClassicEngineOn: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8,
        },

        setForceClassicEngineOff: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8,
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
            dropIndex: 0.01,
        },

        dropIndex: {
            setForceClassicEngineOn: 0.1,
            setForceClassicEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        },
    };

    function teardown(db, collName, cluster) {
        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryFrameworkControl",
            newValue: originalFrameworkControl,
            onMongos: false,
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: "setForceClassicEngineOff",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
