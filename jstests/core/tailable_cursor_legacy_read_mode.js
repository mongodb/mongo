// SERVER-54410: Test that tailable cursors do not throw with legacy read mode.
// @tags: [
//   requires_capped,
//   # This disables all sharding and replica set tests that require usage of sessions. Legacy
//   # queries cannot be used in sessions.
//   assumes_standalone_mongod,
// ]
(function() {
"use strict";

const collName = "tailable_cursor_legacy_read_mode";
const coll = db[collName];
coll.drop();

assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));

const mongo = db.getMongo();
const oldReadMode = mongo.readMode();
try {
    mongo.forceReadMode("legacy");

    assert.commandWorked(coll.insert({a: 1}));
    const results = coll.find({}, {_id: 0}).tailable().toArray();
    assert.eq(results, [{a: 1}]);
} finally {
    mongo.forceReadMode(oldReadMode);
}
})();
