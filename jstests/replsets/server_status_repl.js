import {ReplSetTest} from "jstests/libs/replsettest.js";

let rt = new ReplSetTest({name: "server_status_repl", nodes: 2});
rt.startSet();
rt.initiate();

rt.awaitSecondaryNodes();

let secondary = rt.getSecondary();
let primary = rt.getPrimary();
let testDB = primary.getDB("test");

assert.commandWorked(testDB.createCollection("a"));
assert.commandWorked(testDB.b.insert({}, {writeConcern: {w: 2}}));

let ss = primary.getDB("test").serverStatus({repl: 1});
assert.neq(ss.repl.replicationProgress, null, tojson(ss.repl));

rt.stopSet();
