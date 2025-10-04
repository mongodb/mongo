// Test that dropping the replset oplog, the local database, and the admin database are all
// prohibited in a replset.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let rt = new ReplSetTest({name: "drop_oplog", nodes: 1, oplogSize: 30});

let nodes = rt.startSet();
rt.initiate();
let primary = rt.getPrimary();
let localDB = primary.getDB("local");

let threw = false;

let ret = assert.commandFailed(localDB.runCommand({drop: "oplog.rs"}));
assert.eq("can't drop live oplog while replicating", ret.errmsg);

let dropOutput = localDB.dropDatabase();
assert.eq(dropOutput.ok, 0);
assert.eq(dropOutput.errmsg, "Cannot drop 'local' database while replication is active");

let adminDB = primary.getDB("admin");
dropOutput = adminDB.dropDatabase();
assert.eq(dropOutput.ok, 0);
assert.eq(dropOutput.errmsg, "Dropping the 'admin' database is prohibited.");

let renameOutput = localDB.oplog.rs.renameCollection("poison");
assert.eq(renameOutput.ok, 0);
assert.eq(renameOutput.errmsg, "can't rename live oplog while replicating");

assert.commandWorked(localDB.foo.insert({a: 1}));
renameOutput = localDB.foo.renameCollection("oplog.rs");
assert.eq(renameOutput.ok, 0);
assert.eq(renameOutput.errmsg, "can't rename to live oplog while replicating");
rt.stopSet();
