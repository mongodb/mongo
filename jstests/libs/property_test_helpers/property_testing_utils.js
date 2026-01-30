/*
 * Utility functions to help run a property-based test in a jstest.
 */
import {LeafParameter, leafParametersPerFamily} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

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
    } else if (typeof queryShape === "object" && queryShape !== null) {
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

function createColl(db, coll, isTS = false) {
    const args = isTS ? {timeseries: {timeField: "t", metaField: "m"}} : {};
    assert.commandWorked(db.createCollection(coll.getName(), args));
}
/*
 * Acceptable error codes from creating an index. We could change our model or add filters
 * to the model to remove these cases, but that would cause them to become overcomplicated.
 * In pbt_self_test.js, we assert that the number of indexes created is high enough, to avoid our
 * tests silently erroring too much on index creation.
 */
const okIndexCreationErrorCodes = [
    // Index already exists.
    ErrorCodes.IndexOptionsConflict,
    // Overlapping fields and path collisions in wildcard projection.
    31249,
    31250,
    7246200,
    7246204,
    7246208,
    7246209,
    7246210,
    // For partial index filters, we can sometimes go over the depth limit of the filter. It's
    // difficult to control the exact depth of the filters generated without sacrificing lots of
    // interesting cases, so instead we allow this error.
    ErrorCodes.CannotCreateIndex,
    // Error code when creating specific partial indexes on time-series, for example when the
    // predicate is `{a: {$in: [null]}}`
    5916301,
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
function runProperty(propertyFn, namespaces, workload, sortArrays) {
    let {collSpec, foreignCollSpec, queries, extraParams} = workload;
    const {controlColl, experimentColl, foreignControlColl, foreignExperimentColl} = namespaces;

    function setUpCollection({collection, docs, isTS = false, indexes = []}) {
        assertDropCollection(collection.getDB(), collection.getName());
        createColl(collection.getDB(), collection, isTS);
        assert.commandWorked(collection.insert(docs));
        const createIndex = (indexSpec, num) =>
            assert.commandWorkedOrFailedWithCode(
                collection.createIndex(indexSpec.def, {name: `index_${num}`, ...indexSpec.options}),
                okIndexCreationErrorCodes,
            );
        indexes.forEach(createIndex);
    }

    // Setup the control/experiment collections, define the helper functions, then run the property.
    assert(experimentColl, "experimentColl must be defined");
    setUpCollection({
        collection: experimentColl,
        docs: collSpec.docs,
        isTS: collSpec.isTS,
        indexes: collSpec.indexes,
    });

    if (controlColl) {
        setUpCollection({
            collection: controlColl,
            docs: collSpec.docs,
        });
    }

    if (foreignExperimentColl) {
        assert(foreignCollSpec);
        setUpCollection({
            collection: foreignExperimentColl,
            docs: foreignCollSpec.docs,
            isTS: foreignCollSpec.isTS,
            indexes: foreignCollSpec.indexes,
        });
    }

    if (foreignControlColl) {
        setUpCollection({
            collection: foreignControlColl,
            docs: foreignCollSpec.docs,
        });
    }

    const testHelpers = {
        comp: sortArrays === true ? _resultSetsEqualUnorderedWithUnorderedArrays : _resultSetsEqualUnordered,
        numQueryShapes: queries.length,
        leafParametersPerFamily,
    };

    function getQuery(queryIx, paramIx) {
        assert.lt(queryIx, queries.length);
        const query = queries[queryIx];
        return {pipeline: concreteQueryFromFamily(query.pipeline, paramIx), options: query.options};
    }

    return propertyFn(getQuery, testHelpers, extraParams);
}

/*
 * We need a custom reporter function to get more details on the failure. The default won't show
 * what property failed very clearly, or provide more details beyond the counterexample.
 */
function reporter(propertyFn, namespaces) {
    return function (runDetails) {
        if (runDetails.failed) {
            // Print the fast-check failure summary, the counterexample, and additional details
            // about the property failure.
            jsTest.log.info("Failed property: " + propertyFn.name);
            jsTest.log.info(runDetails);
            const workload = runDetails.counterexample[0];
            jsTest.log.info(workload);
            try {
                jsTest.log.info(runProperty(propertyFn, namespaces, workload));
            } catch (e) {
                jsTest.log.info(e);
            }
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
export function testProperty(propertyFn, namespaces, workloadModel, numRuns, examples, sortArrays) {
    assert.eq(typeof propertyFn, "function");
    assert.eq(typeof numRuns, "number");

    const isValidNamespaceKey = (collName) => {
        switch (collName) {
            case "controlColl":
            case "experimentColl":
            case "foreignControlColl":
            case "foreignExperimentColl":
                return true;
            default:
                return false;
        }
    };
    assert(Object.keys(namespaces).every(isValidNamespaceKey));

    const seed = 4;
    jsTest.log.info("Running property `" + propertyFn.name + "` from test file `" + jsTestName() + "`, seed = " + seed);
    // PBTs can throw (and then catch) exceptions for a few reasons. For example it's hard to model
    // indexes exactly, so we end up trying to create some invalid indexes which throw exceptions.
    // These exceptions make the logs hard to read and can be ignored, so we turn off
    // traceExceptions. These failures are still logged on a single line with the message
    // "Assertion while executing command"
    // True PBT failures (uncaught) are still readable and have stack traces.
    TestData.traceExceptions = false;

    let alwaysPassed = true;
    fc.assert(
        fc.property(workloadModel, (workload) => {
            let result;
            let exception;
            try {
                result = runProperty(propertyFn, namespaces, workload);
            } catch (e) {
                exception = e;
            }

            const passed = !exception && result.passed;

            // If it failed for the first time, print that out so we have the first failure available
            // in case shrinking fails.
            if (!passed && alwaysPassed) {
                jsTest.log.info("The property " + propertyFn.name + " from " + jsTestName() + " failed");
                jsTest.log.info("Initial inputs **before minimization**");
                jsTest.log.info(workload);
                jsTest.log.info("Initial failure details **before minimization**");
                if (exception) {
                    jsTest.log.info(exception);
                } else {
                    jsTest.log.info(result);
                }
                alwaysPassed = false;
            }
            // Rethrow the caught exception if one occurred.
            if (exception) {
                throw exception;
            }
            // Only return if the property passed or not. On failure,
            // `runProperty` is called again and more details are exposed.
            return result.passed;
        }),
        {seed, numRuns, reporter: reporter(propertyFn, namespaces), examples},
    );
}

function isCollTS(collName) {
    const res = assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    const colls = res.cursor.firstBatch;
    assert.eq(colls.length, 1);
    return colls[0].type === "timeseries";
}

export function getPlanCache(coll) {
    const collName = coll.getName();
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
    const priorSettings = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1, internalQueryDisablePlanCache: 1}),
    );
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: "forceClassicEngine",
            internalQueryDisablePlanCache: true,
        }),
    );

    let resultMap = queries.map((query) => {
        assert(Array.isArray(query.pipeline) && typeof query.options === "object");
        return controlColl.aggregate(unoptimize(query.pipeline), query.options).toArray();
    });

    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: priorSettings.internalQueryFrameworkControl,
            internalQueryDisablePlanCache: priorSettings.internalQueryDisablePlanCache,
        }),
    );

    return resultMap;
}
