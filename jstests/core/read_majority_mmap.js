// Ensure read majority works on a standalone node, even with non-snapshotting storage engines.

(function() {
"use strict";

var name = "readMajority";
db = db.getSiblingDB(name);
assert.writeOK(db.foo.insert({x: 3}));

assert.commandWorked(db.foo.runCommand({find: name, readConcern: {level: "majority"}}));

}());
