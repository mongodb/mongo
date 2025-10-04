// Test that test-only set parameters are disabled.
function assertFails(opts) {
    assert.throws(() => MongoRunner.runMongod(opts), [], "Mongod startup up");
}

function assertStarts(opts) {
    const mongod = MongoRunner.runMongod(opts);
    assert(mongod, "Mongod startup up");
    MongoRunner.stopMongod(mongod);
}

TestData.enableTestCommands = false;

// enableTestCommands not specified.
assertFails({
    "setParameter": {
        AlwaysRecordTraffic: "false",
    },
});

// enableTestCommands specified as truthy.
["1", "true"].forEach((v) => {
    assertStarts({
        "setParameter": {
            enableTestCommands: v,
            takeUnstableCheckpointOnShutdown: "false",
        },
    });
});

// enableTestCommands specified as falsy.
["0", "false"].forEach((v) => {
    assertFails({
        "setParameter": {
            enableTestCommands: v,
            AlwaysRecordTraffic: "false",
        },
    });
});
