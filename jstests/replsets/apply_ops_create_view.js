import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const db = replTest.getPrimary().getDB("test");

assert.commandWorked(db.createCollection("bar"));
assert.commandWorked(db.bar.insert({a: 1, b: "hi"}));

const cmd = {
    applyOps: [{op: "c", ns: db + ".$cmd", o: {create: "foo", viewOn: "bar"}}],
};
assert.commandWorked(db.runCommand(cmd), tojson(cmd));
assert.eq(db.foo.findOne({a: 1}).b, "hi");

replTest.stopSet();
