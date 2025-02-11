/*
 * Utility functions to help run a property-based test in a jstest.
 */
import {
    LeafParameter,
    leafParametersPerFamily
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/*
 * Given a query family and an index in [0-numLeafParameters), we replace the leaves of the query
 * with the corresponding constant at that index.
 */
export function concreteQueryFromFamily(queryShape, leafId) {
    if (queryShape instanceof LeafParameter) {
        // We found a leaf, and want to return a concrete constant instead.
        // The leaf node should have one key, and the value should be our constants.
        const vals = queryShape.concreteValues;
        return vals[leafId % vals.length];
    } else if (Array.isArray(queryShape)) {
        // Recurse through the array, replacing each leaf with a value.
        const result = [];
        for (const el of queryShape) {
            result.push(concreteQueryFromFamily(el, leafId));
        }
        return result;
    } else if (typeof queryShape === 'object' && queryShape !== null) {
        // Recurse through the object values and create a new object.
        const obj = {};
        const keys = Object.keys(queryShape);
        for (const key of keys) {
            obj[key] = concreteQueryFromFamily(queryShape[key], leafId);
        }
        return obj;
    }
    return queryShape;
}

function isCollTS(coll) {
    const res =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: coll.getName()}}));
    const colls = res.cursor.firstBatch;
    assert.eq(colls.length, 1);
    return colls[0].type === 'timeseries';
}

export function getPlanCache(coll) {
    const collName = coll.getName();
    if (isCollTS(coll)) {
        return db.system.buckets[collName].getPlanCache();
    }
    return db[collName].getPlanCache();
}

/*
 * Clear any state in the collection (other than data, which doesn't change). Create indexes the
 * test uses, then run the property test.
 *
 * As input, properties a list of query families to use during the property test, and some helpers
 * which include a comparator, and details about how many queries we have.
 *
 * The `getQuery(i, j)` function returns query shape `i` with it's `j`th parameters plugged in.
 * For example, to get different query shapes we would call
 *      getQuery(0, 0)
 *      getQuery(1, 0)
 *      ...
 * To get the same query shape with different parameters, we would call
 *      getQuery(0, 0)
 *      getQuery(0, 1)
 *      ...
 * TODO SERVER-98132 redesign getQuery to be more opaque about how many query shapes and constants
 * there are.
 */
function runProperty(experimentColl, propertyFn, indexes, queries) {
    // Clear all state and create indexes.
    getPlanCache(experimentColl).clear();
    assert.commandWorked(experimentColl.dropIndexes());
    for (const index of indexes) {
        experimentColl.createIndex(index.def, index.options);
    }
    const testHelpers = {
        comp: _resultSetsEqualUnordered,
        numQueryShapes: queries.length,
        leafParametersPerFamily
    };

    function getQuery(queryIx, paramIx) {
        assert.lt(queryIx, queries.length);
        const query = queries[queryIx];
        return concreteQueryFromFamily(query, paramIx);
    }

    return propertyFn(getQuery, testHelpers);
}

/*
 * We need a custom reporter function to get more details on the failure. The default won't show
 * what property failed very clearly, or provide more details beyond the counterexample.
 */
function reporter(experimentColl, propertyFn) {
    return function(runDetails) {
        if (runDetails.failed) {
            // Print the fast-check failure summary, the counterexample, and additional details
            // about the property failure.
            jsTestLog(runDetails);
            const [indexes, queries] = runDetails.counterexample[0];
            jsTestLog({indexes, queries});
            jsTestLog(runProperty(experimentColl, propertyFn, indexes, queries));
            jsTestLog('Failed property: ' + propertyFn.name);
            assert(false);
        }
    };
}

/*
 * Given a property (a JS function), the experiment collection, and execution details, run the given
 * property. We call `runProperty` to clear state and call the property function correctly. On
 * failure, `runProperty` is called again in the reporter, and prints out more details about the
 * failed property.
 */
export function testProperty(
    propertyFn, experimentColl, {aggModel, indexModel, numRuns, numQueriesPerRun}) {
    const nPipelinesArb =
        fc.array(aggModel, {minLength: numQueriesPerRun, maxLength: numQueriesPerRun});
    const nIndexesModel = fc.array(indexModel, {minLength: 0, maxLength: 7});
    const scenarioArb = fc.tuple(nIndexesModel, nPipelinesArb);

    fc.assert(
        fc.property(scenarioArb,
                    ([indexes, pipelines]) => {
                        // Only return if the property passed or not. On failure,
                        // `runProperty` is called again and more details are exposed.
                        return runProperty(experimentColl, propertyFn, indexes, pipelines).passed;
                    }),
        // TODO SERVER-91404 randomize in waterfall.
        {seed: 4, numRuns, reporter: reporter(experimentColl, propertyFn)});
}

function unoptimize(q) {
    return [{$_internalInhibitOptimization: {}}].concat(q);
}

/*
 * Run the given query against the collection with optimizations disabled, no plan cache, using
 * the classic engine.
 */
export function runDeoptimizedQuery(controlColl, query) {
    const initialFrameworkSetting =
        assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
            .internalQueryFrameworkControl;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    getPlanCache(controlColl).clear();
    const controlResults = controlColl.aggregate(unoptimize(query)).toArray();
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: initialFrameworkSetting}));
    return controlResults;
}

/*
 * Default documents to use for the core PBT model schema.
 * TODO SERVER-93816 remove this function and model documents as an arbitrary so that documents can
 * be minimized.
 */
export function defaultPbtDocuments() {
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
    return docs;
}
