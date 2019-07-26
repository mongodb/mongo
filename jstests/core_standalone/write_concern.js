// This test verifies writeConcern behavior on a standalone mongod or embedded mongoed.

(function() {
'use strict';

var col = db.write_concern;
col.drop();

// Supported writeConcern on standalone
assert.commandWorked(col.insert({_id: 0}, {writeConcern: {w: 0}}));
assert.commandWorked(col.insert({_id: 1}, {writeConcern: {w: 1}}));
assert.commandWorked(col.insert({_id: "majority"}, {writeConcern: {w: "majority"}}));

// writeConcern: 2 should not work on standalone
assert.writeError(col.insert({_id: 2}, {writeConcern: {w: 2}}), "expected writeConcern: 2 to fail");
})();