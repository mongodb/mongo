/**
 * Tests that the {shardingState: 1} command doesn't return an entry for each collection when
 * sharding isn't enabled.
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB("test");
    assert.commandWorked(db.runCommand({create: "mycoll"}));

    const res = assert.commandWorked(db.adminCommand({shardingState: 1}));
    assert(!res.hasOwnProperty("versions"),
           "unexpectedly found version info about collections: " + tojson(res));

    rst.stopSet();
})();
