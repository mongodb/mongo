(function() {
    "use strict";

    var storageEngine = jsTest.options().storageEngine || "wiredTiger";
    if (storageEngine !== "wiredTiger") {
        print('Skipping test because storageEngine is not "wiredTiger"');
        return;
    }

    var rst = new ReplSetTest({
        nodes: [
            {"enableMajorityReadConcern": ""},
            {"enableMajorityReadConcern": "false"},
            {"enableMajorityReadConcern": "true"}
        ]
    });
    rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();

    rst.getPrimary().getDB("test").getCollection("test").insert({});
    rst.awaitReplication();

    // Node 0 is using the default, which is `enableMajorityReadConcern: true`. Thus a majority
    // read should succeed.
    assert.commandWorked(rst.nodes[0].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}}));
    // Node 1 disables majorite reads. Check for the appropriate error code.
    var againstDisabledNode = rst.nodes[1].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}});
    assert.eq(againstDisabledNode["ok"], 0);
    assert.eq(againstDisabledNode["code"], 148);
    // Same as Node 0.
    assert.commandWorked(rst.nodes[2].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}}));

    rst.stopSet();
})();
