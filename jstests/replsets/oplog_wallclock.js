// oplog should contain the field "wt" with wallClock timestamps.
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getLatestOp} from "jstests/replsets/rslib.js";

let assertLastOplogHasWT = function (primary, msg) {
    const opLogEntry = getLatestOp(primary);
    assert(opLogEntry.hasOwnProperty("wall"), "oplog entry must contain wt field: " + tojson(opLogEntry));
};

let name = "wt_test_coll";
let replSet = new ReplSetTest({nodes: 1, oplogSize: 2});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let collection = primary.getDB("test").getCollection(name);

assert.commandWorked(collection.insert({_id: 1, val: "x"}));
assertLastOplogHasWT(primary, "insert");

assert.commandWorked(collection.update({_id: 1}, {val: "y"}));
assertLastOplogHasWT(primary, "update");

assert.commandWorked(collection.remove({_id: 1}));
assertLastOplogHasWT(primary, "remove");

replSet.stopSet();
