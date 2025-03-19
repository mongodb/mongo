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

function createColl(coll, isTS = false) {
    const db = coll.getDB();
    const args = isTS ? {timeseries: {timeField: 't', metaField: 'm'}} : {};
    assert.commandWorked(db.createCollection(coll.getName(), args));
}

// Error codes from creating an index that are acceptable. We could change our model or add filters
// to the model to remove these cases, but that would cause them to become overcomplicated.
const okIndexCreationErrorCodes = [
    // Index already exists.
    85,
    // Overlapping fields and path collisions in wildcard projection.
    31249,
    31250,
    7246200,
    7246204,
    7246208,
];

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
function runProperty(propertyFn, namespaces, collectionSpec, queries) {
    const {controlColl, experimentColl} = namespaces;

    // Setup the control/experiment collections, define the helper functions, then run the property.
    if (controlColl) {
        assert(controlColl.drop());
        createColl(controlColl);
        assert.commandWorked(controlColl.insert(collectionSpec.docs));
    }

    assert(experimentColl.drop());
    createColl(experimentColl, collectionSpec.isTS);
    assert.commandWorked(experimentColl.insert(collectionSpec.docs));
    collectionSpec.indexes.forEach((indexSpec, num) => {
        const name = "index_" + num;
        assert.commandWorkedOrFailedWithCode(
            experimentColl.createIndex(indexSpec.def, Object.extend(indexSpec.options, {name})),
            okIndexCreationErrorCodes);
    });

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
function reporter(propertyFn, namespaces) {
    return function(runDetails) {
        if (runDetails.failed) {
            // Print the fast-check failure summary, the counterexample, and additional details
            // about the property failure.
            jsTestLog(runDetails);
            const {collSpec, queries} = runDetails.counterexample[0];
            jsTestLog({collSpec, queries});
            jsTestLog(runProperty(propertyFn, namespaces, collSpec, queries));
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
    propertyFn, namespaces, {collModel, aggModel}, {numRuns, numQueriesPerRun}) {
    const nPipelinesModel =
        fc.array(aggModel, {minLength: numQueriesPerRun, maxLength: numQueriesPerRun});
    const scenarioArb = fc.record({collSpec: collModel, queries: nPipelinesModel});

    fc.assert(fc.property(scenarioArb,
                          ({collSpec, queries}) => {
                              // Only return if the property passed or not. On failure,
                              // `runProperty` is called again and more details are exposed.
                              return runProperty(propertyFn, namespaces, collSpec, queries).passed;
                          }),
              // TODO SERVER-91404 randomize in waterfall.
              {seed: 4, numRuns, reporter: reporter(propertyFn, namespaces)});
}

function isCollTS(collName) {
    const res = assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    const colls = res.cursor.firstBatch;
    assert.eq(colls.length, 1);
    return colls[0].type === 'timeseries';
}

export function getPlanCache(coll) {
    const collName = coll.getName();
    if (isCollTS(collName)) {
        return db.system.buckets[collName].getPlanCache();
    }
    return db[collName].getPlanCache();
}

function unoptimize(q) {
    return [{$_internalInhibitOptimization: {}}].concat(q);
}

/*
 * Runs the given function with the following settings:
 * - execution framework set to classic engine
 * - plan cache disabled
 * - pipeline optimizations disabled
 * Returns a map from the position of the query in the list to the result documents.
 */
export function runDeoptimized(controlColl, queries) {
    // The `internalQueryDisablePlanCache` prevents queries from getting cached, but it does not
    // prevent queries from using existing cache entries. To fully ignore the cache, we clear it
    // and then set the `internalQueryDisablePlanCache` knob.
    getPlanCache(controlColl).clear();
    const db = controlColl.getDB();
    const priorSettings = assert.commandWorked(db.adminCommand(
        {getParameter: 1, internalQueryFrameworkControl: 1, internalQueryDisablePlanCache: 1}));
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: 'forceClassicEngine',
        internalQueryDisablePlanCache: true
    }));

    const resultMap = queries.map(query => controlColl.aggregate(unoptimize(query)).toArray());

    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: priorSettings.internalQueryFrameworkControl,
        internalQueryDisablePlanCache: priorSettings.internalQueryDisablePlanCache
    }));

    return resultMap;
}
