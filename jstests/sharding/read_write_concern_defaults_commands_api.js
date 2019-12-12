// Tests the basic API of the getDefaultRWConcern and setDefaultRWConcern commands against different
// topologies.
//
// @tags: [requires_fcv_44]
(function() {
"use strict";

// Asserts a set/get default RWC command response contains the expected fields. Assumes a default
// read or write concern has been set previously.
function verifyResponseFields(res, {expectRC, expectWC}) {
    // These fields are always set once a read or write concern has been set at least once.
    const expectedFields = ["epoch", "setTime", "localSetTime"];
    const unexpectedFields = [];

    if (expectRC) {
        expectedFields.push("defaultReadConcern");
    } else {
        unexpectedFields.push("defaultReadConcern");
    }

    if (expectWC) {
        expectedFields.push("defaultWriteConcern");
    } else {
        unexpectedFields.push("defaultWriteConcern");
    }

    assert.hasFields(res, expectedFields);
    unexpectedFields.forEach(field => {
        assert(!res.hasOwnProperty(field),
               `response unexpectedly had field '${field}', res: ${tojson(res)}`);
    });
}

function verifyDefaultRWCommandsInvalidInput(conn) {
    //
    // Test invalid parameters for getDefaultRWConcern.
    //

    // Invalid inMemory.
    assert.commandFailedWithCode(conn.adminCommand({getDefaultRWConcern: 1, inMemory: "true"}),
                                 ErrorCodes.TypeMismatch);

    //
    // Test invalid parameters for setDefaultRWConcern.
    //

    // Must include either wc or rc.
    assert.commandFailedWithCode(conn.adminCommand({setDefaultRWConcern: 1}), ErrorCodes.BadValue);

    // Invalid write concern.
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: 1}),
        ErrorCodes.TypeMismatch);

    // w less than 1.
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 0},
    }),
                                 ErrorCodes.BadValue);

    // Invalid read concern.
    assert.commandFailedWithCode(conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: 1}),
                                 ErrorCodes.TypeMismatch);

    // Non-existent level.
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "dummy"}}),
        ErrorCodes.FailedToParse);

    // Unsupported level.
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "linearizable"}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "snapshot"}}),
        ErrorCodes.BadValue);

    // Fields other than level.
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local", afterClusterTime: Timestamp(50, 1)}
    }),
                                 ErrorCodes.BadValue);
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "snapshot", atClusterTime: Timestamp(50, 1)}
    }),
                                 ErrorCodes.BadValue);
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local", afterOpTime: {ts: Timestamp(50, 1), t: 1}}
    }),
                                 ErrorCodes.BadValue);
}

// Sets a default read and write concern.
function setDefaultRWConcern(conn) {
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local"},
        defaultWriteConcern: {w: 1}
    }));
}

// Unsets the default read and write concerns.
function unsetDefaultRWConcern(conn) {
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {}}));
}

// Verifies no fields are returned if neither a default read nor write concern has been set.
function verifyDefaultResponses(conn) {
    const res = assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1}));
    const inMemoryRes =
        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true}));

    const unexpectedFields =
        ["defaultReadConcern", "defaultWriteConcern", "epoch", "setTime", "localSetTime"];
    unexpectedFields.forEach(field => {
        assert(!res.hasOwnProperty(field),
               `response unexpectedly had field '${field}', res: ${tojson(res)}`);
        assert(!inMemoryRes.hasOwnProperty(field),
               `inMemory=true response unexpectedly had field '${field}', res: ${tojson(res)}`);
    });
}

function verifyDefaultRWCommandsValidInput(conn) {
    //
    // Test parameters for getDefaultRWConcern.
    //

    // No parameters is allowed.
    assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1}));

    // inMemory parameter is allowed.
    assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true}));
    assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: false}));

    //
    // Test parameters for setDefaultRWConcern.
    //

    // Setting only rc is allowed.
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}));
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "majority"}}));

    // Setting only wc is allowed.
    assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}));
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1, j: false}}));
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}}));

    // Setting both wc and rc is allowed.
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        defaultReadConcern: {level: "local"}
    }));

    // Empty write concern is allowed.
    assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {}}));

    // Empty read concern is allowed.
    assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {}}));
}

function verifyDefaultRWCommandsSuccessfulResponses(conn) {
    //
    // Test responses for getDefaultRWConcern.
    //

    // When neither read nor write concern is set.
    unsetDefaultRWConcern(conn);
    verifyResponseFields(assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})),
                         {expectRC: false, expectWC: false});
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true})),
        {expectRC: false, expectWC: false});

    // When only read concern is set.
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}, defaultWriteConcern: {}}));
    verifyResponseFields(assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})),
                         {expectRC: true, expectWC: false});
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true})),
        {expectRC: true, expectWC: false});

    // When only write concern is set.
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {w: 1}}));
    verifyResponseFields(assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})),
                         {expectRC: false, expectWC: true});
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true})),
        {expectRC: false, expectWC: true});

    // When both read and write concern are set.
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local"},
        defaultWriteConcern: {w: 1}
    }));
    verifyResponseFields(assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})),
                         {expectRC: true, expectWC: true});
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory: true})),
        {expectRC: true, expectWC: true});

    //
    // Test responses for setDefaultRWConcern.
    //

    // When unsetting both read and write concern.
    setDefaultRWConcern(conn);
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand(
            {setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {}})),
        {expectRC: false, expectWC: false});

    // When unsetting only read concern.
    setDefaultRWConcern(conn);
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {}})),
        {expectRC: false, expectWC: true});

    // When unsetting only write concern.
    setDefaultRWConcern(conn);
    verifyResponseFields(
        assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {}})),
        {expectRC: true, expectWC: false});

    // When setting only write concern.
    unsetDefaultRWConcern(conn);
    verifyResponseFields(assert.commandWorked(conn.adminCommand(
                             {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}})),
                         {expectRC: false, expectWC: true});

    // When setting only read concern.
    unsetDefaultRWConcern(conn);
    verifyResponseFields(assert.commandWorked(conn.adminCommand(
                             {setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}})),
                         {expectRC: true, expectWC: false});

    // When setting both read and write concern.
    unsetDefaultRWConcern(conn);
    verifyResponseFields(assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local"},
        defaultWriteConcern: {w: 1}
    })),
                         {expectRC: true, expectWC: true});
}

// Verifies the error code returned by connections to nodes that do not support the get/set default
// rw concern commands.
function verifyDefaultRWCommandsFailWithCode(conn, {failureCode}) {
    assert.commandFailedWithCode(conn.adminCommand({getDefaultRWConcern: 1}), failureCode);
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}),
        failureCode);
}

jsTestLog("Testing standalone mongod...");
{
    const standalone = MongoRunner.runMongod();

    // Standalone node fails.
    verifyDefaultRWCommandsFailWithCode(standalone, {failureCode: 51300});

    MongoRunner.stopMongod(standalone);
}

jsTestLog("Testing standalone replica set...");
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    // Primary succeeds.
    verifyDefaultResponses(rst.getPrimary());
    verifyDefaultRWCommandsValidInput(rst.getPrimary());
    verifyDefaultRWCommandsInvalidInput(rst.getPrimary());
    verifyDefaultRWCommandsSuccessfulResponses(rst.getPrimary());

    // Secondary succeeds.
    assert.commandWorked(rst.getSecondary().adminCommand({getDefaultRWConcern: 1}));
    // TODO SERVER-44890 Assert setDefaultRWConcern fails with NotMaster instead.
    assert.commandWorked(rst.getSecondary().adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}));

    rst.stopSet();
}

jsTestLog("Testing sharded cluster...");
{
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

    // Mongos succeeds.
    verifyDefaultResponses(st.s);
    verifyDefaultRWCommandsValidInput(st.s);
    verifyDefaultRWCommandsInvalidInput(st.s);
    verifyDefaultRWCommandsSuccessfulResponses(st.s);

    // Shard node fails.
    verifyDefaultRWCommandsFailWithCode(st.rs0.getPrimary(), {failureCode: 51301});
    verifyDefaultRWCommandsFailWithCode(st.rs0.getSecondary(), {failureCode: 51301});

    // Config server primary succeeds.
    verifyDefaultRWCommandsValidInput(st.configRS.getPrimary());
    verifyDefaultRWCommandsInvalidInput(st.configRS.getPrimary());
    verifyDefaultRWCommandsSuccessfulResponses(st.configRS.getPrimary());

    // Config server secondary succeeds.
    assert.commandWorked(st.configRS.getSecondary().adminCommand({getDefaultRWConcern: 1}));
    // TODO SERVER-44890 Assert setDefaultRWConcern fails instead.
    assert.commandWorked(st.configRS.getSecondary().adminCommand(
        {setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}));

    st.stop();
}
})();
