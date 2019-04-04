/**
 * Tests that merizoS rejects 'aggregate' commands which explicitly set any of the
 * parameters that merizoS uses internally when communicating with the shards.
 */
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1, enableMajorityReadConcern: ''}});

    const merizosDB = st.s0.getDB(jsTestName());
    const merizosColl = merizosDB[jsTestName()];

    assert.commandWorked(merizosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    // Test that command succeeds when no internal options have been specified.
    assert.commandWorked(
        merizosDB.runCommand({aggregate: merizosColl.getName(), pipeline: [], cursor: {}}));

    // Test that the command fails if we have 'needsMerge: false' without 'fromMongos'.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, needsMerge: false}),
        ErrorCodes.FailedToParse);

    // Test that the command fails if we have 'needsMerge: true' without 'fromMongos'.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, needsMerge: true}),
        ErrorCodes.FailedToParse);

    // Test that 'fromMongos: true' cannot be specified in a command sent to merizoS.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, fromMongos: true}),
        51089);

    // Test that 'fromMongos: false' can be specified in a command sent to merizoS.
    assert.commandWorked(merizosDB.runCommand(
        {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, fromMongos: false}));

    // Test that the command fails if we have 'needsMerge: true' with 'fromMongos: false'.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMongos: false
    }),
                                 51089);

    // Test that the command fails if we have 'needsMerge: true' with 'fromMongos: true'.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMongos: true
    }),
                                 51089);

    // Test that 'needsMerge: false' can be specified in a command sent to merizoS along with
    // 'fromMongos: false'.
    assert.commandWorked(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: false,
        fromMongos: false
    }));

    // Test that 'mergeByPBRT: true' cannot be specified in a command sent to merizoS.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, mergeByPBRT: true}),
        51089);

    // Test that 'mergeByPBRT: false' can be specified in a command sent to merizoS.
    assert.commandWorked(merizosDB.runCommand(
        {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, mergeByPBRT: false}));

    // Test that the 'exchange' parameter cannot be specified in a command sent to merizoS.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        exchange: {policy: 'roundrobin', consumers: NumberInt(2)}
    }),
                                 51028);

    // Test that the command fails when all internal parameters have been specified.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMongos: true,
        mergeByPBRT: true,
        exchange: {policy: 'roundrobin', consumers: NumberInt(2)}
    }),
                                 51028);

    // Test that the command fails when all internal parameters but exchange have been specified.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMongos: true,
        mergeByPBRT: true
    }),
                                 51089);

    st.stop();
})();
