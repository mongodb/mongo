// Ensure read majority works on a standalone node, even with non-snapshotting storage engines.

(function() {
"use strict";

var name = "read_majority_mmap";
assert.writeOK(db[name].insert({x: 3}));

assert.commandWorked(db.runCommand({find: name, readConcern: {level: "majority"}}));

}());
