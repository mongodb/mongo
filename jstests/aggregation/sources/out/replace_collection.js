/**
 * Tests the behavior of legacy $out.
 *
 * This test assumes that collections are not implicitly sharded, since $out is prohibited if the
 * output collection is sharded.
 */
import {dropWithoutImplicitRecreate} from "jstests/aggregation/extras/merge_helpers.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.source;
const targetCollName = "target";
coll.drop();

dropWithoutImplicitRecreate(targetCollName);
const pipeline = [{$out: targetCollName}];

//
// Test $out with a non-existent output collection.
//
assert.commandWorked(coll.insert({_id: 0}));
coll.aggregate(pipeline);
const targetColl = db.target;
assert.eq(1, targetColl.find().itcount());

// Test $out with a non-existent database. Sharded pass throughs will implicitly shard accessed
// collections so we skip this test here cause you can't out to a sharded collecti
const destDB = db.getSiblingDB("outDifferentDB");
destDB.dropDatabase();
if (!FixtureHelpers.isMongos(db)) {
    coll.aggregate({$out: {db: destDB.getName(), coll: destDB.outDifferentColl.getName()}});
    assert.eq(1, destDB.outDifferentColl.find().itcount());
}

//
// Test $out with an existing output collection.
//
coll.aggregate(pipeline);
assert.eq(1, targetColl.find().itcount());

//
// Test that $out will preserve the indexes and options of the output collection.
//
dropWithoutImplicitRecreate(targetCollName);
assert.commandWorked(db.createCollection(targetColl.getName(), {validator: {a: {$gt: 0}}}));
assert.commandWorked(targetColl.createIndex({a: 1}));

coll.drop();
assert.commandWorked(coll.insert({a: 1}));

coll.aggregate(pipeline);
assert.eq(1, targetColl.find().itcount());
assert.eq(2, targetColl.getIndexes().length);

const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
assert.commandWorked(listColl);
assert.eq({a: {$gt: 0}}, listColl.cursor.firstBatch[0].options["validator"]);

//
// Test that $out fails if it violates a unique index constraint.
//
coll.drop();
assert.commandWorked(coll.insert([{_id: 0, a: 0}, {_id: 1, a: 0}]));
dropWithoutImplicitRecreate(targetCollName);
assert.commandWorked(targetColl.createIndex({a: 1}, {unique: true}));

assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

// Rerun a similar test, except populate the target collection with a document that conflics
// with one out of the pipeline. In this case, there is no unique key violation since the target
// collection will be dropped before renaming the source collection.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 0}));
targetColl.remove({});
assert.commandWorked(targetColl.insert({_id: 1, a: 0}));

coll.aggregate(pipeline);
assert.eq(1, targetColl.find().itcount());
assert.eq(2, targetColl.getIndexes().length);
