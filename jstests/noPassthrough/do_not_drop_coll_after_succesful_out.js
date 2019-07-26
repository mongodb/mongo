// Confirms that there's no attempt to drop a temp collection after $out is performed.
(function() {
"use strict";

// Prevent the mongo shell from gossiping its cluster time, since this will increase the amount
// of data logged for each op.
TestData.skipGossipingClusterTime = true;

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const testDB = conn.getDB("test");
const coll = testDB.do_not_drop_coll_after_succesful_out;

assert.commandWorked(coll.insert({a: 1}));

assert.commandWorked(testDB.setLogLevel(2, "command"));
assert.commandWorked(testDB.adminCommand({clearLog: "global"}));

coll.aggregate([{$out: coll.getName() + "_out"}]);
const log = assert.commandWorked(testDB.adminCommand({getLog: "global"})).log;

for (let i = 0; i < log.length; ++i) {
    const line = log[i];
    assert.eq(line.indexOf("drop test.tmp.agg_out"), -1, line);
}

MongoRunner.stopMongod(conn);
})();
