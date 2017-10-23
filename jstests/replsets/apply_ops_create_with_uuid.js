(function() {
    // Test applyOps behavior for collection creation with explicit UUIDs.
    "use strict";

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const db = replTest.getPrimary().getDB('test');

    const uuid = UUID();
    // Two applyOps to create a foo collection with given uuid, one each for 'test' and 'test2' dbs.
    var ops = (uuid => ["test", "test2"].map(db => {
        return {op: "c", ns: db + ".$cmd", ui: uuid, o: {create: "foo"}};
    }))(uuid);

    function checkUUID(coll, uuid) {
        const cmd = {listCollections: 1, filter: {name: coll}};
        const res = assert.commandWorked(db.runCommand(cmd), tojson(cmd));
        assert.eq(res.cursor.firstBatch[0].info.uuid,
                  uuid,
                  tojson(cmd) + " did not return expected uuid: " + tojson(res));
    }

    jsTestLog("Create a test.foo collection with uuid " + uuid + " through applyOps.");
    let cmd = {applyOps: [ops[0]]};
    let res = assert.commandWorked(db.runCommand(cmd), tojson(cmd));

    // Check that test.foo has the expected UUID.
    jsTestLog("Check that test.foo has UUID " + uuid);
    checkUUID("foo", uuid);

    // Change the ops to refer to bar, instead of foo. Command should still work, renaming the
    // collection.  Second command should fail as it tries to associate the "test2.foo" name with
    // an existing collection in the "test" database. This must fail.
    jsTestLog("Create test.bar and try to create test2.foo collections with the same UUID.");
    ops[0].o.create = "bar";
    res = assert.commandFailed(db.runCommand({applyOps: ops}));
    assert.eq(res.results,
              [true, false],
              "expected first operation " + tojson(ops[0]) + " to succeed, and second operation " +
                  tojson(ops[1]) + " to fail, got " + tojson(res));

    jsTestLog("Check that test.bar has UUID " + uuid);
    checkUUID("bar", uuid);
    jsTestLog("Check that test.foo no longer exists");
    assert.eq(db.getCollectionInfos({name: "foo"}).length,
              0,
              "expected foo collection to no longer exist");
}());
