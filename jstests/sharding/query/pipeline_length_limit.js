/**
 * Confirms that the limit on number of aggregragation pipeline stages is respected.
 * @tags: [requires_fcv_49]
 */
(function() {
"use strict";

function testLimits(testDB, lengthLimit) {
    const maxLength = lengthLimit;
    const tooLarge = lengthLimit + 1;

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(maxLength).fill({$project: {_id: 1}})
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(tooLarge).fill({$project: {_id: 1}})
    }),
                                 ErrorCodes.FailedToParse);
    testDB.setLogLevel(1);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{
            $lookup:
                {from: "test", as: "as", pipeline: new Array(maxLength).fill({$project: {_id: 1}})}
        }]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{
            $lookup:
                {from: "test", as: "as", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})}
        }]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [
            {$unionWith: {coll: "test", pipeline: new Array(maxLength).fill({$project: {_id: 1}})}}
        ]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline:
            [{$unionWith: {coll: "test", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})}}]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{$facet: {foo: new Array(maxLength).fill({$project: {_id: 1}})}}]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{$facet: {foo: new Array(tooLarge).fill({$project: {_id: 1}}), bar: []}}]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand(
        {update: "test", updates: [{q: {}, u: new Array(maxLength).fill({$project: {_id: 1}})}]}));
    assert.commandFailedWithCode(testDB.runCommand({
        update: "test",
        updates: [{q: {}, u: new Array(tooLarge).fill({$project: {_id: 1}})}]
    }),
                                 ErrorCodes.FailedToParse);
}

function runTest(lengthLimit, mongosConfig = {}, mongodConfig = {}) {
    const st = new ShardingTest(
        {shards: 2, rs: {nodes: 1}, other: {mongosOptions: mongosConfig, rsOptions: mongodConfig}});

    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: "test.foo", key: {_id: "hashed"}}));

    let mongosDB = st.s0.getDB("test");
    assert.commandWorked(mongosDB.test.insert([{}, {}, {}, {}]));

    // Run test against mongos.
    testLimits(mongosDB, lengthLimit);

    // Run test against shard.
    let shard0DB = st.rs0.getPrimary().getDB("test");
    testLimits(shard0DB, lengthLimit);

    st.stop();
}

// Test default pipeline length limit.
runTest(1000);

// Test with modified pipeline length limit.
runTest(50,
        {setParameter: {internalPipelineLengthLimit: 50}},
        {setParameter: {internalPipelineLengthLimit: 50}});
})();
