/**
 * join_plan_cache.js
 *
 * High-concurrency join plan cache workload against the TPC-H scale-0.1 dataset.
 *
 * Each thread independently transitions between two states: query (99%) and ddl (1%). Query
 * iterations run an official or a fuzzed TPC-H plan-stability query. DDL iterations issue
 * an index DDL (hide/unhide, drop+recreate, create new indexes) against the
 * TPC-H collections.
 *
 * This workload uses the dedicated TPC-H dataset so it can not be run in the usual concurrency suites.
 *
 * @tags: [
 *   requires_external_data_source,
 *   incompatible_with_concurrency_simultaneous,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_91,
 *   requires_sbe,
 *   requires_getmore,
 *   assumes_balancer_off,
 * ]
 */

import {joinPlanCacheStats, joinPlanCacheOccupancy} from "jstests/libs/query/join_utils.js";
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {populateTPCHDataset} from "jstests/libs/query/tpch_dataset.js";
import {commands as officialCommands} from "jstests/query_golden/test_inputs/plan_stability_pipelines_tpch_official.js";
import {commands as fuzzedCommands} from "jstests/query_golden/test_inputs/plan_stability_pipelines_tpch_fuzzed.js";

// This test has a lot of failing statements, so we do not print a JS stack trace for each.
TestData.traceExceptions = false;

/**
 *  A local version of isSanitizerBuild() that does not require a db argument, as db is not available in FSM
 */
function isSanitizerBuild() {
    const buildInfo = getBuildInfo();
    const ccflags = buildInfo.buildEnvironment?.ccflags ?? "";
    const sanitizeMatch = /-fsanitize=([^\s]+) /.exec(ccflags);
    const sanitizeFlags = sanitizeMatch ? sanitizeMatch[1] : "";
    return (
        !/(\s|^)-O2(\s|$)/.test(ccflags) ||
        /address/.test(sanitizeFlags) ||
        /leak/.test(sanitizeFlags) ||
        /thread/.test(sanitizeFlags) ||
        /undefined/.test(sanitizeFlags)
    );
}

const kThreadCount = isSanitizerBuild() ? 25 : 100;
jsTest.log.info("Using kThreadCount", {kThreadCount});

// Abort queries after 1 second.
const kMaxTimeMS = 1000;

// Reduce the frequency of the slow query log messages.
const kSlowms = 10000;

const kTpchCollections = [
    "customer",
    "orders",
    "lineitem",
    "part",
    "partsupp",
    "supplier",
    "nation",
    "region",
];

// When building a new index, build only indexes that would be relevant to the TPC-H queries.
// Only build indexes on the smaller collections in order to avoid excessive index build times.
const kNewIndexSpecs = [
    {coll: "customer", key: {c_mktsegment: 1}},
    {coll: "customer", key: {c_name: 1}},
    {coll: "customer", key: {c_acctbal: 1}},
    {coll: "part", key: {p_type: 1}},
    {coll: "part", key: {p_mfgr: 1}},
    {coll: "part", key: {p_name: 1}},
    {coll: "part", key: {p_container: 1}},
    {coll: "part", key: {p_size: 1}},
    {coll: "part", key: {p_retailprice: 1}},
    {coll: "supplier", key: {s_acctbal: 1}},
    {coll: "supplier", key: {s_name: 1}},
    {coll: "nation", key: {n_name: 1}},
    {coll: "region", key: {r_name: 1}},
];

// Error codes that are acceptable and expected and will not cause the test to fail.
const kAcceptableDdlErrorCodes = [
    ErrorCodes.IndexNotFound,
    ErrorCodes.IndexBuildAborted,
    ErrorCodes.IndexAlreadyExists,
    ErrorCodes.IndexOptionsConflict,
    ErrorCodes.IndexKeySpecsConflict,
    ErrorCodes.ConflictingOperationInProgress,
    ErrorCodes.NoMatchingDocument,
    ErrorCodes.IndexBuildAlreadyInProgress,
    ErrorCodes.CannotCreateIndex,
];

const kAcceptableQueryErrorCodes = interruptedQueryErrors.concat([
    ErrorCodes.MaxTimeMSExpired,
    ErrorCodes.ExceededTimeLimit,
    ErrorCodes.LockTimeout,
    ErrorCodes.StaleConfig,
    ErrorCodes.NetworkInterfaceExceededTimeLimit,
    3994303, // TODO(SERVER-134942):$expr comparison predicates on multikey paths cannot use an index
    12926303, // TODO(SERVER-135049): planFromCache failed for cached access path
]);

// First row count per query that was observed, used to assert that later executions return the same number of rows.
const kObservedRowCounts = {};

function listUserIndexes(coll) {
    return coll.getIndexes().filter((idx) => idx.name !== "_id_");
}

function pickRandom(arr) {
    return arr[Random.randInt(arr.length)];
}

function randomSleep() {
    // Sleep a random duration between 0.5s and 1.5s.
    sleep(500 + Random.randInt(1000));
}

export const $config = (function () {
    const data = {
        originalJoinParams: {},
        originalSlowms: {},
        joinCacheStatsBefore: {},
        tpchCollections: kTpchCollections,
        newIndexSpecs: kNewIndexSpecs,
        queryErrors: {},
    };

    const states = {
        ddl: function ddl(db, collName) {
            this.runDdl(db);
        },

        query: function query(db, collName) {
            if (Random.rand() < 0.5) {
                this.runQuery(db, officialCommands, "official");
            } else {
                this.runQuery(db, fuzzedCommands, "fuzzed");
            }
        },
    };

    function runDdl(db) {
        const op = Random.randInt(4);
        if (op === 0) {
            this.hideOrUnhideIndex(db);
        } else if (op === 1) {
            this.dropAndRecreateIndex(db);
        } else if (op === 2) {
            // Make an index temporarily multikey. Currently causes a tripwire assertion.
            // TODO(SERVER-134942)
            // this.addAndRemoveArrayValue(db);
        } else {
            this.createNewIndex(db);
        }
    }

    function hideOrUnhideIndex(db) {
        const collName = pickRandom(this.tpchCollections);
        const indexes = listUserIndexes(db[collName]);
        if (indexes.length === 0) {
            return;
        }
        const index = pickRandom(indexes);
        if (index.hidden) {
            assert.commandWorkedOrFailedWithCode(
                db[collName].unhideIndex(index.name),
                kAcceptableDdlErrorCodes,
            );
        } else {
            assert.commandWorkedOrFailedWithCode(
                db[collName].hideIndex(index.name),
                kAcceptableDdlErrorCodes,
            );
        }
    }

    function dropAndRecreateIndex(db) {
        const collName = pickRandom(this.tpchCollections);
        const indexes = listUserIndexes(db[collName]);
        if (indexes.length === 0) {
            return;
        }
        const index = pickRandom(indexes);
        assert.commandWorkedOrFailedWithCode(
            db.runCommand({dropIndexes: collName, index: index.name}),
            kAcceptableDdlErrorCodes,
        );
        randomSleep();
        const options = Object.extend({}, index);
        delete options.key;
        delete options.ns;
        delete options.indexBuildInfo;
        assert.commandWorkedOrFailedWithCode(
            db[collName].createIndex(index.key, options),
            kAcceptableDdlErrorCodes,
        );
    }

    /**
     * Make an index temporarily multikey by adding an array value. Then remove the value
     * so that the index can be dropped and recreated as a non-multikey one.
     */
    function addAndRemoveArrayValue(db) {
        const collName = pickRandom(this.tpchCollections);
        const indexes = listUserIndexes(db[collName]);
        if (indexes.length === 0) {
            return;
        }
        const index = pickRandom(indexes);
        const fields = Object.keys(index.key);
        if (fields.length === 0) {
            return;
        }
        const field = pickRandom(fields);

        // Only the chosen indexed field is populated; _id is set so we can delete the same doc.
        const doc = {[field]: []};
        if (field !== "_id") {
            doc._id = new ObjectId();
        }
        const insertRes = db.runCommand({insert: collName, documents: [doc]});
        if (
            !insertRes.ok ||
            insertRes.n === 0 ||
            (insertRes.writeErrors && insertRes.writeErrors.length > 0)
        ) {
            return;
        }

        randomSleep();

        // Remove the array value.
        db.runCommand({delete: collName, deletes: [{q: {_id: doc._id}, limit: 1}]});

        // Restore the multikey=false property by dropping and recreating the index.
        assert.commandWorkedOrFailedWithCode(
            db.runCommand({dropIndexes: collName, index: index.name}),
            kAcceptableDdlErrorCodes,
        );
        const options = Object.extend({}, index);
        delete options.key;
        delete options.ns;
        delete options.indexBuildInfo;
        assert.commandWorkedOrFailedWithCode(
            db[collName].createIndex(index.key, options),
            kAcceptableDdlErrorCodes,
        );
    }

    function createNewIndex(db) {
        const spec = pickRandom(this.newIndexSpecs);
        assert.commandWorkedOrFailedWithCode(
            db[spec.coll].createIndex(spec.key),
            kAcceptableDdlErrorCodes,
        );
    }

    function runQuery(db, commands, commandSet) {
        const cmd = commands[Random.randInt(commands.length)];
        const idx = String(cmd.idx);

        try {
            // Run the query and assert that it returns the same number of rows as prior executions.
            const nReturned = db[cmd.aggregate]
                .aggregate(cmd.pipeline, {maxTimeMS: kMaxTimeMS})
                .itcount();
            const key = `${commandSet}:${idx}`;
            if (!kObservedRowCounts.hasOwnProperty(key)) {
                kObservedRowCounts[key] = nReturned;
            } else {
                assert.eq(
                    nReturned,
                    kObservedRowCounts[key],
                    `result set length changed across executions of query ${idx}`,
                    {commandSet, idx},
                );
            }
        } catch (e) {
            if (
                (e.code === undefined || !kAcceptableQueryErrorCodes.includes(e.code)) &&
                !JSON.stringify(e).includes("operation exceeded time limit")
            ) {
                throw e;
            }
        }
    }

    const transitions = {
        ddl: {ddl: 0.01, query: 0.99},
        query: {ddl: 0.01, query: 0.99},
    };

    function setup(db, collName, cluster) {
        populateTPCHDataset("0.1", db.getName(), {compact: false});

        // Enable join optimization and join plan cache.
        cluster.executeOnMongodNodes((nodeDb) => {
            const orig = assert.commandWorked(
                nodeDb.adminCommand({
                    getParameter: 1,
                    internalEnableJoinOptimization: 1,
                    internalEnableJoinPlanCache: 1,
                }),
            );
            this.originalJoinParams[nodeDb.getMongo().host] = {
                internalEnableJoinOptimization: orig.internalEnableJoinOptimization,
                internalEnableJoinPlanCache: orig.internalEnableJoinPlanCache,
            };
            assert.commandWorked(
                nodeDb.adminCommand({
                    setParameter: 1,
                    internalEnableJoinOptimization: true,
                    internalEnableJoinPlanCache: true,
                }),
            );
        });

        // Set slowms to a higher threshold to avoid spamming the log
        const setSlowms = (nodeDb) => {
            const orig = assert.commandWorked(nodeDb.runCommand({profile: -1}));
            this.originalSlowms[nodeDb.getMongo().host] = {
                slowms: orig.slowms,
                profile: orig.was,
            };
            assert.commandWorked(nodeDb.runCommand({profile: orig.was, slowms: kSlowms}));
        };
        cluster.executeOnMongodNodes(setSlowms);
        cluster.executeOnMongosNodes(setSlowms);

        this.joinCacheStatsBefore = joinPlanCacheStats(db);
    }

    function teardown(db, collName, cluster) {
        try {
            const after = joinPlanCacheStats(db);
            const occupancy = joinPlanCacheOccupancy(db);
            const hits = after.hits - this.joinCacheStatsBefore.hits;
            const misses = after.misses - this.joinCacheStatsBefore.misses;
            const invalidations = after.invalidations - this.joinCacheStatsBefore.invalidations;
            const lookups = hits + misses;
            const hitRate = lookups === 0 ? 0 : hits / lookups;
            const invalidationRate = lookups === 0 ? 0 : invalidations / lookups;

            jsTest.log.info("Join plan cache utilization", {
                hits,
                misses,
                invalidations,
                lookups,
                hitRate,
                invalidationRate,
                numEntries: occupancy.numEntries,
                estimatedSizeBytes: occupancy.estimatedSizeBytes,
            });

            cluster.executeOnMongodNodes((nodeDb) => {
                const host = nodeDb.getMongo().host;
                const snapshot = joinPlanCacheStats(nodeDb);
                const nodeOccupancy = joinPlanCacheOccupancy(nodeDb);
                jsTest.log.info("Join plan cache utilization per mongod", {
                    host,
                    hits: snapshot.hits,
                    misses: snapshot.misses,
                    invalidations: snapshot.invalidations,
                    numEntries: nodeOccupancy.numEntries,
                    estimatedSizeBytes: nodeOccupancy.estimatedSizeBytes,
                });
            });

            assert.gt(lookups, 0, "expected join plan cache lookups during the workload", {
                hits,
                misses,
                invalidations,
            });
        } finally {
            const restoreSlowms = (nodeDb) => {
                const orig = this.originalSlowms[nodeDb.getMongo().host];
                if (orig === undefined) {
                    return;
                }
                assert.commandWorked(
                    nodeDb.runCommand({profile: orig.profile, slowms: orig.slowms}),
                );
            };
            cluster.executeOnMongodNodes(restoreSlowms);
            cluster.executeOnMongosNodes(restoreSlowms);

            cluster.executeOnMongodNodes((nodeDb) => {
                const orig = this.originalJoinParams[nodeDb.getMongo().host];
                assert.commandWorked(
                    nodeDb.adminCommand({
                        setParameter: 1,
                        internalEnableJoinOptimization: orig.internalEnableJoinOptimization,
                        internalEnableJoinPlanCache: orig.internalEnableJoinPlanCache,
                    }),
                );
            });
        }
    }

    return {
        threadCount: kThreadCount,
        iterations: 500,
        startState: "query",
        states: states,
        transitions: transitions,
        data: Object.extend(data, {
            runDdl: runDdl,
            hideOrUnhideIndex: hideOrUnhideIndex,
            dropAndRecreateIndex: dropAndRecreateIndex,
            addAndRemoveArrayValue: addAndRemoveArrayValue,
            createNewIndex: createNewIndex,
            runQuery: runQuery,
        }),
        setup: setup,
        teardown: teardown,
    };
})();
