//
// Tests valid cases for creation of 2dsphere index
//

import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

let coll = db.getCollection("twodspherevalid");

// Valid index
coll.drop();
assert.commandWorked(coll.createIndex({geo: "2dsphere", other: 1}, add2dsphereVersionIfNeeded()));

// Valid index
coll.drop();
assert.commandWorked(coll.createIndex({geo: "2dsphere", other: 1, geo2: "2dsphere"}, add2dsphereVersionIfNeeded()));

// Invalid index, using hash with 2dsphere
coll.drop();
assert.commandFailed(coll.createIndex({geo: "2dsphere", other: "hash"}));

// Invalid index, using 2d with 2dsphere
coll.drop();
assert.commandFailed(coll.createIndex({geo: "2dsphere", other: "2d"}));

jsTest.log("Success!");

// Ensure the empty collection is gone, so that small_oplog passes.
coll.drop();
