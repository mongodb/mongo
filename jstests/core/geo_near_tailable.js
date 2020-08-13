// @tags: [
//   requires_capped,
//   sbe_incompatible,
// ]
//
// Tests that combine $geoNear and tailable cursors.
//
(function() {
"use strict";

let cmdRes;
const collName = 'geo_near_tailable';
const cappedCollName = 'geo_near_tailable_capped';

// Avoid using the drop() shell helper here in order to avoid "implicit collection recreation"
// which can happen when this test runs in certain passthroughs. For details, see
// "jstests/libs/override_methods/implicitly_shard_accessed_collections.js".
db.runCommand({drop: collName});
db.runCommand({drop: cappedCollName});
assert.commandWorked(db.createCollection(collName));
assert.commandWorked(db.createCollection(cappedCollName, {capped: true, size: 10000}));

// Error when tailable option is used with NEAR.
cmdRes = db.runCommand({find: collName, filter: {a: {$geoNear: [1, 2]}}, tailable: true});
assert.commandFailedWithCode(cmdRes, ErrorCodes.BadValue);
cmdRes = db.runCommand({find: cappedCollName, filter: {a: {$geoNear: [1, 2]}}, tailable: true});
assert.commandFailedWithCode(cmdRes, ErrorCodes.BadValue);
})();
