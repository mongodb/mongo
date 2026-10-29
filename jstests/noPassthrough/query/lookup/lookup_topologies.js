/**
 * Tests correctness of the $lookup across combinations of topologies and different
 * $lookup variants.
 *
 * Lookup shapes can be executed as a top-level pipeline, or as a nested pipeline
 * in another lookup. Topologies tested for each local and foreign collection are
 * unsharded, unsharded moved off of primary, and sharded.
 *
 * @tags: [requires_sharding]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

const kDbName = jsTestName();
const kForeignDocs = [
    {_id: 0, join_key: 0, value: "a"},
    {_id: 1, join_key: 1, value: "b"},
];
const kLocalDocs = [
    {_id: 0, join_key: 0},
    {_id: 1, join_key: 0},
    {_id: 2, join_key: 0},
    {_id: 3, join_key: 1},
    {_id: 4, join_key: 1},
    {_id: 5, join_key: 1},
];

function setUpCollections(db) {
    assert(db.local.drop());
    assert(db.foreign.drop());
    assert(db.foreignView.drop());
    assert(db.driver.drop());
    assert.commandWorked(db.foreign.insertMany(kForeignDocs));
    assert.commandWorked(db.local.insertMany(kLocalDocs));
    // When we run the lookup in a nested pipeline, this will drive the top-level
    // lookup. Only used for that one case.
    assert.commandWorked(db.driver.insertOne({}));
    assert.commandWorked(
        db.createView("foreignView", "foreign", [{$addFields: {rand: {$rand: {}}}}]),
    );
}

// All of the lookup queries we run should produce the same results. These
// are the expected results.
const kExpectedResults = kLocalDocs.map(({join_key}) => ({
    join_key,
    join_result: kForeignDocs.filter((doc) => doc.join_key === join_key),
}));

function makeTopologies(st) {
    return [
        function unshardedOnPrimary(ns) {},
        function unshardedMovedOffPrimary(ns) {
            assert.commandWorked(
                st.s.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}),
            );
        },
        function sharded(ns) {
            assert.commandWorked(st.s.getCollection(ns).createIndex({_id: "hashed"}));
            assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));
        },
    ];
}

function assertCorrectResult(result, foreignNs, debugInfo) {
    // If we queried against the view, we'll have an additional "rand" field that
    // we need to remove before asserting the results. We want to make sure the
    // view behaved correctly and produced unique values.
    if (foreignNs === "foreignView") {
        const randValues = new Set();
        for (const doc of result) {
            for (const joinedDoc of doc.join_result) {
                randValues.add(joinedDoc.rand);
                delete joinedDoc.rand;
            }
        }
        // "rand" should be unique. $rand can produce 2^53 different unique values, so
        // it is highly unlikely we'll see a duplicate.
        assert.eq(randValues.size, result.length, {randValues, result, ...debugInfo});
    }

    assert.sameMembers(result, kExpectedResults, debugInfo);
}

// Generate the different $lookup shapes.
function* generateLookups() {
    for (const foreignNs of ["foreign", "foreignView"]) {
        yield {
            foreignNs,
            lookup: {
                $lookup: {
                    from: foreignNs,
                    localField: "join_key",
                    foreignField: "join_key",
                    as: "join_result",
                },
            },
        };
        // Equivalent stage to above, but uses a let variable.
        yield {
            foreignNs,
            lookup: {
                $lookup: {
                    from: foreignNs,
                    let: {join_key_alias: "$join_key"},
                    pipeline: [{$match: {$expr: {$eq: ["$join_key", "$$join_key_alias"]}}}],
                    as: "join_result",
                },
            },
        };
    }
}

// Runs all lookup combinations against the given db.
function runAllCombinationsAgainstDb(db, debugInfo) {
    for (const {foreignNs, lookup} of generateLookups()) {
        const pipeline = [lookup, {$project: {_id: 0, join_key: 1, join_result: 1}}];

        // Test the $lookup ran at the top-level.
        const debugInfoTopLevel = {...debugInfo, pipeline};
        assertCorrectResult(db.local.aggregate(pipeline).toArray(), foreignNs, debugInfoTopLevel);

        // Test the $lookup ran as a subpipeline for another $lookup, and check that
        // the doc nested inside the top-level result is what we expect.
        const nestedPipeline = [{$limit: 1}, {$lookup: {from: "local", pipeline, as: "n"}}];
        const debugInfoNested = {...debugInfo, pipeline: nestedPipeline};
        const res = db.driver.aggregate(nestedPipeline).toArray();
        assert.eq(res.length, 1, {res});
        assertCorrectResult(res[0].n, foreignNs, debugInfoNested);
    }
}

function testStandalone() {
    const conn = MongoRunner.runMongod();
    try {
        const db = conn.getDB(kDbName);
        setUpCollections(db);
        runAllCombinationsAgainstDb(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

function testShardedScenarios() {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
    });
    try {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
        );
        const db = st.s.getDB(kDbName);
        if (isSlowBuild(db)) {
            // This part of the test runs combinations of topologies and lookup shapes which could
            // be slow on debug builds.
            jsTest.log.info("Skipping " + jsTestName() + " sharded scenarios on slow builds.");
            return;
        }
        const topologies = makeTopologies(st);

        // Iterate through combinations of local/foreign topologies, and assert
        // correctness for each one.
        for (const localTopology of topologies) {
            for (const foreignTopology of topologies) {
                setUpCollections(db);
                localTopology(`${kDbName}.local`);
                foreignTopology(`${kDbName}.foreign`);
                const debugInfo = {
                    localTopology: localTopology.name,
                    foreignTopology: foreignTopology.name,
                };
                runAllCombinationsAgainstDb(db, debugInfo);
            }
        }
    } finally {
        st.stop();
    }
}

testStandalone();
testShardedScenarios();
