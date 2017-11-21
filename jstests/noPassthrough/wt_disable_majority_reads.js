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

    assert.commandWorked(rst.nodes[0].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}}));
    var againstDisabledNode = rst.nodes[1].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}});
    assert.eq(againstDisabledNode["ok"], 0);
    assert.eq(againstDisabledNode["code"], 148);
    assert.commandWorked(rst.nodes[2].getDB("test").runCommand(
        {"find": "test", "readConcern": {"level": "majority"}}));

    rst.stopSet();
})();
