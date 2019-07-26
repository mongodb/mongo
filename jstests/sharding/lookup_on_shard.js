// Test that a pipeline with a $lookup stage on a sharded foreign collection may be run on a mongod.
(function() {

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

const sharded = new ShardingTest({mongos: 1, shards: 2});

setParameterOnAllHosts(
    DiscoverTopology.findNonConfigNodes(sharded.s), "internalQueryAllowShardedLookup", true);

assert.commandWorked(sharded.s.adminCommand({enableSharding: "test"}));
sharded.ensurePrimaryShard('test', sharded.shard0.shardName);

const coll = sharded.s.getDB('test').mainColl;
const foreignColl = sharded.s.getDB('test').foreignColl;
const smallColl = sharded.s.getDB("test").smallColl;

const nDocsMainColl = 10;
const nDocsForeignColl = 2 * nDocsMainColl;

for (let i = 0; i < nDocsMainColl; i++) {
    assert.commandWorked(coll.insert({_id: i, collName: "mainColl", foreignId: i}));

    assert.commandWorked(
        foreignColl.insert({_id: 2 * i, key: i, collName: "foreignColl", data: "hello-0"}));
    assert.commandWorked(
        foreignColl.insert({_id: 2 * i + 1, key: i, collName: "foreignColl", data: "hello-1"}));
}
assert.commandWorked(smallColl.insert({_id: 0, collName: "smallColl"}));

const runTest = function() {
    (function testSingleLookupFromShard() {
        // Run a pipeline which must be merged on a shard. This should force the $lookup (on
        // the sharded collection) to be run on a mongod.
        pipeline = [
                {
                  $lookup: {
                      localField: "foreignId",
                      foreignField: "key",
                      from: "foreignColl",
                      as: "foreignDoc"
                  }
                },
                {$_internalSplitPipeline: {mergeType: "anyShard"}}
            ];

        const results = coll.aggregate(pipeline).toArray();
        assert.eq(results.length, nDocsMainColl);
        for (let i = 0; i < results.length; i++) {
            assert.eq(results[i].foreignDoc.length, 2, results[i]);
        }
    })();

    (function testMultipleLookupsFromShard() {
        // Run two lookups in a row (both on mongod).
        pipeline = [
                {
                  $lookup: {
                      localField: "foreignId",
                      foreignField: "key",
                      from: "foreignColl",
                      as: "foreignDoc"
                  }
                },
                {
                  $lookup: {
                      from: "smallColl",
                      as: "smallCollDocs",
                      pipeline: [],
                  }
                },
                {$_internalSplitPipeline: {mergeType: "anyShard"}}
            ];
        const results = coll.aggregate(pipeline).toArray();
        assert.eq(results.length, nDocsMainColl);
        for (let i = 0; i < results.length; i++) {
            assert.eq(results[i].foreignDoc.length, 2, results[i]);
            assert.eq(results[i].smallCollDocs.length, 1, results[i]);
        }
    })();

    (function testUnshardedLookupWithinShardedLookup() {
        // Pipeline with unsharded $lookup inside a sharded $lookup.
        pipeline = [
                {
                  $lookup: {
                      from: "foreignColl",
                      as: "foreignDoc",
                      pipeline: [
                          {$lookup: {from: "smallColl", as: "doc", pipeline: []}},
                      ],
                  }
                },
                {$_internalSplitPipeline: {mergeType: "anyShard"}}
            ];
        const results = coll.aggregate(pipeline).toArray();

        assert.eq(results.length, nDocsMainColl);
        for (let i = 0; i < results.length; i++) {
            assert.eq(results[i].foreignDoc.length, nDocsForeignColl);
            for (let j = 0; j < nDocsForeignColl; j++) {
                // Each document pulled from the foreign collection should have one document
                // from "smallColl."
                assert.eq(results[i].foreignDoc[j].collName, "foreignColl");

                // TODO SERVER-39016: Once a mongod is able to target the primary shard when
                // reading from a non-sharded collection this should always work. Until then,
                // the results of the query depend on which shard is chosen as the merging
                // shard. If the primary shard is chosen, we'll get the correct results (and
                // correctly find a document in "smallColl"). Otherwise if the merging shard is
                // not the primary shard, the merging shard will attempt to do a local read (on
                // an empty/non-existent collection), which will return nothing.
                if (results[i].foreignDoc[j].doc.length === 1) {
                    assert.eq(results[i].foreignDoc[j].doc[0].collName, "smallColl");
                } else {
                    assert.eq(results[i].foreignDoc[j].doc.length, 0);
                }
            }
        }
    })();
};

jsTestLog("Running test with neither collection sharded");
runTest();

jsTestLog("Running test with foreign collection sharded");
sharded.shardColl(
    "foreignColl",
    {_id: 1},  // shard key
    {_id: 5},  // split
    {_id: 5},  // move
    "test",    // dbName
    true       // waitForDelete
);
runTest();

jsTestLog("Running test with main and foreign collection sharded");
sharded.shardColl(
    "mainColl",
    {_id: 1},  // shard key
    {_id: 5},  // split
    {_id: 5},  // move
    "test",    // dbName
    true       // waitForDelete
);
runTest();

sharded.stop();
})();
