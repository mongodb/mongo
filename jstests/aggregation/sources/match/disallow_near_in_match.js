/**
 * Test that $near queries are disallowed in $match stages.
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.getCollection("no_near_in_match");
coll.drop();

// Create indexes that could satisfy various $near queries.
assert.commandWorked(coll.createIndex({point2d: "2d"}));
assert.commandWorked(coll.createIndex({point2dsphere: "2dsphere"}));

// Populate the collection so that successful queries can return at least one result.
assert.commandWorked(coll.insert({point2d: [0.25, 0.35]}));
assert.commandWorked(coll.insert({point2dsphere: [0.25, 0.35]}));

const nearQuery = {
    point2d: {$near: [0, 0]},
};
const nearSphereQuery = {
    point2dsphere: {$nearSphere: [0, 0]},
};
const geoNearQuery = {
    point2d: {$geoNear: [0, 0]},
};

// Test that normal finds return a result.
assert.eq(1, coll.find(nearQuery).count());
assert.eq(1, coll.find(nearSphereQuery).count());
assert.eq(1, coll.find(geoNearQuery).count());

// Test that we refuse to run $match with a near query.
assertErrorCode(coll, {$match: nearQuery}, 5626500);
assertErrorCode(coll, {$match: nearSphereQuery}, 5626500);
assertErrorCode(coll, {$match: geoNearQuery}, 5626500);
