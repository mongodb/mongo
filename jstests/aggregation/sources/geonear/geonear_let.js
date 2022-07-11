/**
 * Tests that $geoNear allows for variable in the 'near' argument.
 * $geoNear is not allowed in a facet, even in a lookup.
 * @tags: [
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");

const collName = jsTest.name();
const coll = db[collName];

coll.drop();
assert.commandWorked(coll.insert({_id: 0, location: {type: "Point", coordinates: [10, 20]}}));
assert.commandWorked(coll.insert({_id: 1, location: {type: "Point", coordinates: [8, 22]}}));

assert.commandWorked(coll.createIndex({location: "2dsphere"}));

/**
 * Compare pipelines with constant and variable 'near' argument.
 */
function compareResults(nearArgument) {
    const pipeline = [{$geoNear: {near: "$$pt", distanceField: "distance"}}];
    const res = coll.aggregate(pipeline, {let : {pt: nearArgument}}).toArray();

    const constRes =
        coll.aggregate([{$geoNear: {near: nearArgument, distanceField: "distance"}}]).toArray();

    assert.eq(res.length, constRes.length);
    for (let i = 0; i < res.length; i++) {
        assert.eq(res[i].distance, constRes[i].distance);
    }
}

compareResults({type: "Point", coordinates: [10, 22]});

// Let variable in $lookup pipeline.
const geo2 = db.geonear_let2;
geo2.drop();
assert.commandWorked(geo2.insert({_id: 5, location: {type: "Point", coordinates: [10, 21]}}));
assert.commandWorked(geo2.insert({_id: 6, location: {type: "Point", coordinates: [11, 21]}}));

let pipelineLookup = [
	{$lookup: {
            from: coll.getName(),
            let: {pt: "$location"},
            pipeline: [{$geoNear: {near: "$$pt", distanceField: "distance"}}, {$match: {_id: 0}}],
            as: "joinedField"
	}}];

const lookupRes = geo2.aggregate(pipelineLookup).toArray();
assert.eq(lookupRes.length, 2);
// Make sure the computed distance uses the location field in the current document in the outer
// collection.
assert.neq(lookupRes[0].joinedField[0].distance, lookupRes[1].joinedField[0].distance);

// With legacy geometry.

coll.drop();
assert.commandWorked(coll.insert({_id: 0, location: [10, 20]}));
assert.commandWorked(coll.insert({_id: 1, location: [8, 22]}));
coll.createIndex({location: "2d"});

compareResults([10, 22]);

// Error checks.
let err = assert.throws(
    () => coll.aggregate([{$geoNear: {near: "$$pt", distanceField: "distance"}}], {let : {pt: 5}}));
assert.commandFailedWithCode(err, 5860401);

err = assert.throws(() => coll.aggregate([{$geoNear: {near: "$$pt", distanceField: "distance"}}],
                                         {let : {pt: "abc"}}));
assert.commandFailedWithCode(err, 5860401);
}());
