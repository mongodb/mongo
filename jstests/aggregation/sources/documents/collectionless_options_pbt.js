/**
 * Test mongod doesn't crash under varying collectionless aggregations and
 * command options.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   do_not_wrap_aggregations_in_facets,
 *   query_intensive_pbt,
 * ]
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because isSlowBuild");
    quit();
}

const seed = 4;
const resumeToken = {$recordId: NumberLong(1)};
const testDB = db.getSiblingDB(jsTestName());
const adminDB = db.getSiblingDB("admin");

testDB.c.drop();
assert.commandWorked(testDB.c.insert({_id: 1, a: 1}));

const sources = [
    {runDB: testDB, target: "c", stages: [{$changeStream: {}}]},
    {runDB: testDB, target: 1, stages: [{$documents: [{a: 1}, {a: 2}]}]},
    {runDB: adminDB, target: 1, stages: [{$currentOp: {}}]},
    {runDB: testDB, target: 1, stages: [{$listLocalSessions: {}}]},
    {runDB: adminDB, target: 1, stages: [{$querySettings: {}}]},
    {runDB: adminDB, target: 1, stages: [{$listCatalog: {}}]},
    {runDB: adminDB, target: 1, stages: [{$listMqlEntities: {entityType: "aggregationStages"}}]},
    {runDB: adminDB, target: 1, stages: [{$queryStats: {}}]},
    {runDB: adminDB, target: 1, stages: [{$listClusterCatalog: {}}]},
    {runDB: adminDB, target: 1, stages: [{$listSampledQueries: {}}]},
    {runDB: testDB, target: "c", stages: []},
];

const cmdOpts = [
    {$_resumeAfter: resumeToken},
    {$_startAt: resumeToken},
    {$_requestResumeToken: true},
    {$_requestReshardingResumeToken: true},
    {$_externalDataSources: []},
    {$_isClusterQueryWithoutShardKeyCmd: true},
    {$_isHybridSearch: true},
    {$_passthroughToShard: {shard: "shard0"}},
    {$_translatedForViewlessTimeseries: true},
];

const caseArb = fc.record({src: fc.constantFrom(...sources), opts: fc.subarray(cmdOpts)});

fc.assert(
    fc.property(fc.array(caseArb, {minLength: 1, maxLength: 25}), (cases) => {
        for (const {src, opts} of cases) {
            // We are only verifying the server does not crash.
            src.runDB.runCommand(Object.assign({aggregate: src.target, pipeline: src.stages, cursor: {}}, ...opts));
        }
        return true;
    }),
    {seed, numRuns: 100},
);

assert.commandWorked(adminDB.runCommand({ping: 1}));
