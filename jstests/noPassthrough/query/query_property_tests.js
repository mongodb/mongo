/*
 * Generates random query shapes, and checks that these queries satisfy a set of properties. These
 * properties include caching rules, correctness against classic engine collscans, and others.
 */

import {listOfIndexesModel} from "jstests/libs/property_test_helpers/query_models.js";
import {propertyTests} from "jstests/libs/property_test_helpers/query_properties.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// Force classic control collection with no indexes.
const controlConn =
    MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: "forceClassicEngine"}});
assert.neq(controlConn, null, "mongod failed to start up");
const controlDb = controlConn.getDB(jsTestName());
const controlColl = controlDb.control_collection;
controlColl.drop();

const experimentConn = MongoRunner.runMongod();
assert.neq(experimentConn, null, "mongod failed to start up");
const experimentDb = experimentConn.getDB(jsTestName());
const experimentPlainColl = experimentDb.experiment_collection;
const experimentTsColl = experimentDb.experiment_ts_collection;

const datePrefix = 1680912440;
const alphabet = 'abcdefghijklmnopqrstuvwxyz';
const docs = [];
let id = 0;
for (let m = 0; m < 10; m++) {
    let currentDate = 0;
    for (let i = 0; i < 10; i++) {
        docs.push({
            _id: id,
            t: new Date(datePrefix + currentDate - 100),
            m: {m1: m, m2: 2 * m},
            array: [i, i + 1, 2 * i],
            a: NumberInt(10 - i),
            b: alphabet.charAt(i)
        });
        currentDate += 25;
        id += 1;
    }
}

// Setup our control collection, as well as our experiment control and experiment TS collections.
assert.commandWorked(
    controlDb.adminCommand({configureFailPoint: 'disablePipelineOptimization', mode: 'alwaysOn'}));
assert.commandWorked(experimentDb.createCollection(experimentTsColl.getName(), {
    timeseries: {timeField: 't', metaField: 'm'},
}));

assert.commandWorked(controlColl.insert(docs));
assert.commandWorked(experimentPlainColl.insert(docs));
assert.commandWorked(experimentTsColl.insert(docs));

function getPlanCache(coll) {
    assert(coll === experimentPlainColl || coll === experimentTsColl);
    if (coll === experimentTsColl) {
        return experimentDb.system.buckets.experiment_ts_collection.getPlanCache();
    }
    return coll.getPlanCache();
}

// Clear any state in the collection (other than data, which doesn't change). Create indexes the
// test uses, then run the property test.
function runProperty(propertyFn, isTs, indexes, pipelines) {
    const experimentColl = isTs ? experimentTsColl : experimentPlainColl;
    // Clear all state and create indexes.
    getPlanCache(experimentColl).clear();
    assert.commandWorked(experimentColl.dropIndexes());
    for (const index of indexes) {
        experimentColl.createIndex(index.def, index.options);
    }
    const testHelpers = {
        comp: _resultSetsEqualUnordered,
        experimentDb,
        controlColl,
        getPlanCache,
        serverStatus: () => experimentDb.serverStatus()
    };
    return propertyFn(experimentColl, pipelines, testHelpers);
}

// We need a custom reporter function to get more details on the failure. The default won't show
// what property failed very clearly, or provide more details beyond the counterexample.
function reporter(propertyFn) {
    return function(runDetails) {
        if (runDetails.failed) {
            // Print the fast-check failure summary, the counterexample, and additional details
            // about the property failure.
            jsTestLog(runDetails);
            const [isTs, indexes, pipelines] = runDetails.counterexample[0];
            jsTestLog({isTs, indexes, pipelines});
            jsTestLog(runProperty(propertyFn, isTs, indexes, pipelines));
            jsTestLog('Failed property: ' + propertyFn.name);
            assert(false);
        }
    };
}

function isWildcardIndex(index) {
    for (const field of Object.keys(index.def)) {
        if (field.includes('$**')) {
            return true;
        }
    }
    return false;
}

function isHashedIndex(index) {
    for (const field of Object.keys(index.def)) {
        if (index.def[field] === 'hashed') {
            return true;
        }
    }
    return false;
}

function isMultikey(index) {
    for (const field of Object.keys(index.def)) {
        if (field === 'array') {
            return true;
        }
    }
    return false;
}

// Test a property, given the property function (from query_properties.js). We construct a pipeline
// model from some metadata about the property, and call `runProperty` to clear state and call the
// property function correctly. On failure, `runProperty` is called again in the reporter, and
// prints out more details about the failed property.
function testProperty(propertyFn, aggModel, numQueriesNeeded, numRuns) {
    const nPipelinesArb =
        fc.array(aggModel, {minLength: numQueriesNeeded, maxLength: numQueriesNeeded});

    const scenarioArb = fc.tuple(fc.boolean(), listOfIndexesModel, nPipelinesArb)
                            .filter(([isTs, indexes, pipelines]) => {
                                // --- Permanent conditions ---
                                // TS collections don't support Wildcard, Hashed, or Sparse indexes.
                                // Also multikey indexes on measurement fields are not allowed.
                                if (isTs) {
                                    for (const index of indexes) {
                                        if (index.options.sparse || isWildcardIndex(index) ||
                                            isHashedIndex(index) || isMultikey(index)) {
                                            return false;
                                        }
                                    }
                                }
                                return true;
                            });

    fc.assert(fc.property(scenarioArb,
                          ([isTs, indexes, pipelines]) => {
                              // Only return if the property passed or not. On failure,
                              // `runProperty` is called again and more details are exposed.
                              return runProperty(propertyFn, isTs, indexes, pipelines).passed;
                          }),
              // TODO SERVER-91404 randomize in waterfall.
              {seed: 4, numRuns, reporter: reporter(propertyFn)});
}

for (const {propertyFn, aggModel, numQueriesNeeded, numRuns} of propertyTests) {
    testProperty(propertyFn, aggModel, numQueriesNeeded, numRuns);
}

MongoRunner.stopMongod(controlConn);
MongoRunner.stopMongod(experimentConn);
