// Tests that you cannot use aggregation to output to a system collection.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For 'assertErrorCode'.
load("jstests/libs/fixture_helpers.js");      // For 'isMongos'.

const input = db.no_output_to_system;
input.drop();
assert.commandWorked(input.insert({_id: 0}));

// Ensure that aggregation can't output to system collection.
const outputInSystem = db.system.no_output_to_system;
assertErrorCode(input, {$out: outputInSystem.getName()}, 17385);
assert(!collectionExists(outputInSystem));

assertErrorCode(input, {$merge: outputInSystem.getName()}, 31319);
assert(!collectionExists(outputInSystem));

// Ensure that aggregation can't output to the 'admin' database.
const admin = db.getSiblingDB('admin');
const outputToAdmin = admin.users;

assertErrorCode(input, {$merge: {into: {db: 'admin', coll: outputToAdmin.getName()}}}, 31320);
assert(!collectionExists(outputToAdmin));

assertErrorCode(input, {$out: {db: 'admin', coll: outputToAdmin.getName()}}, 31321);
assert(!collectionExists(outputToAdmin));

// Ensure that aggregation can't output to the 'config' database.
const config = db.getSiblingDB('config');
const outputToConfig = config.output;

assertErrorCode(input, {$merge: {into: {db: 'config', coll: outputToConfig.getName()}}}, 31320);
assert(!collectionExists(outputToConfig));

assertErrorCode(input, {$out: {db: 'config', coll: outputToConfig.getName()}}, 31321);
assert(!collectionExists(outputToConfig));

// Ensure that aggregation can't output to the 'local' database.
// Only test if local exists (i.e. we are not on mongos).
if (!FixtureHelpers.isMongos(db)) {
    const local = db.getSiblingDB('local');

    // Every mongod has this collection.
    const outputToLocal = local.startup_log;

    assertErrorCode(input, {$merge: {into: {db: 'local', coll: outputToLocal.getName()}}}, 31320);

    // $out allows for the source collection to be the same as the destination collection.
    assertErrorCode(outputToLocal, {$out: outputToLocal.getName()}, 31321);
}
})();
