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

    // Test that the command fails if we have 'needsMerge: false' without 'fromMerizos'.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, needsMerge: false}),
        ErrorCodes.FailedToParse);

    // Test that the command fails if we have 'needsMerge: true' without 'fromMerizos'.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, needsMerge: true}),
        ErrorCodes.FailedToParse);

    // Test that 'fromMerizos: true' cannot be specified in a command sent to merizoS.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, fromMerizos: true}),
        51089);

    // Test that 'fromMerizos: false' can be specified in a command sent to merizoS.
    assert.commandWorked(merizosDB.runCommand(
        {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, fromMerizos: false}));

    // Test that the command fails if we have 'needsMerge: true' with 'fromMerizos: false'.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMerizos: false
    }),
                                 51089);

    // Test that the command fails if we have 'needsMerge: true' with 'fromMerizos: true'.
    assert.commandFailedWithCode(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: true,
        fromMerizos: true
    }),
                                 51089);

    // Test that 'needsMerge: false' can be specified in a command sent to merizoS along with
    // 'fromMerizos: false'.
    assert.commandWorked(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [],
        cursor: {},
        needsMerge: false,
        fromMerizos: false
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
        fromMerizos: true,
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
        fromMerizos: true,
        mergeByPBRT: true
    }),
                                 51089);

    st.stop();
})();
