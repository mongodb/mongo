/**
 * Check that the special $-prefixed field names $ref, $id and $db all work in expressions, $group,
 * and $lookup.
 *
 * Uses $lookup, which doesn't support sharded foreign collection.
 * DBRef fields aren't supported in agg pre 4.4.
 * @tags: [assumes_unsharded_collection, requires_fcv_44]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For anyEq.
const coll = db.dbref_in_expression;
const otherColl = db.dbref_in_expression_2;

coll.drop();
otherColl.drop();

assert.commandWorked(otherColl.insert({_id: "id0", x: 1}));
assert.commandWorked(otherColl.insert({_id: "id1", x: 2}));

assert.commandWorked(coll.insert({
    _id: 0,
    link: new DBRef(otherColl.getName(), "id0", db.getName()),
    linkArray: [
        new DBRef(otherColl.getName(), "id0", db.getName()),
        new DBRef(otherColl.getName(), "id1", db.getName())
    ]
}));

function projectOnlyPipeline(projection) {
    const aggRes = coll.aggregate({$project: projection}).toArray();
    const findRes = coll.find({}, projection).toArray();
    assert.sameMembers(findRes, aggRes);
    return aggRes;
}

// Refer to a DBRef sub-field in a projection.
assert.eq(projectOnlyPipeline({refVal: "$link.$ref"}), [{_id: 0, refVal: otherColl.getName()}]);
assert.eq(projectOnlyPipeline({refVal: "$linkArray.$ref"}),
          [{_id: 0, refVal: [otherColl.getName(), otherColl.getName()]}]);

assert.eq(projectOnlyPipeline({idVal: "$link.$id"}), [{_id: 0, idVal: "id0"}]);
assert.eq(projectOnlyPipeline({idVal: "$linkArray.$id"}), [{_id: 0, idVal: ["id0", "id1"]}]);

assert.eq(projectOnlyPipeline({idVal: "$link.$db"}), [{_id: 0, idVal: db.getName()}]);
assert.eq(projectOnlyPipeline({idVal: "$linkArray.$db"}),
          [{_id: 0, idVal: [db.getName(), db.getName()]}]);

// Use a DBRef sub-field in an expression.
assert.eq(projectOnlyPipeline({idLen: {$strLenCP: "$link.$id"}}), [{_id: 0, idLen: "id0".length}]);

// Project away DBRef values.
assert.eq(projectOnlyPipeline({link: {$ref: 0}, linkArray: 0}),
          [{_id: 0, link: {$id: "id0", $db: db.getName()}}]);

assert.eq(projectOnlyPipeline({link: 0, linkArray: {$id: 0}}), [{
              _id: 0,
              linkArray: [
                  {$ref: otherColl.getName(), $db: db.getName()},
                  {$ref: otherColl.getName(), $db: db.getName()}
              ]
          }]);

// Assigning to a DBRef field.
assert.eq(projectOnlyPipeline({link: {$ref: 1, $id: 1, $db: "someOtherDB"}}),
          [{_id: 0, link: new DBRef(otherColl.getName(), "id0", "someOtherDB")}]);

// While not a 'feature' we advertise, it is allowed to assign to top-level DBRef fields.
assert.eq(projectOnlyPipeline({$ref: "$link.$ref"}), [{_id: 0, $ref: otherColl.getName()}]);

// One cannot refer to a top-level DBRef field, however, as it will be interpreted as a variable
// dereference.
const err = assert.throws(() => coll.aggregate({$project: {x: "$$ref"}}).toArray());
assert.eq(err.code, 17276);

// It can be accessed through $$ROOT, however.
assert.eq(coll.aggregate([
                  // Rather than go through the trouble of inserting a document with a top-level
                  // $-prefixed field, create one in an intermediate $project stage.
                  {$project: {"$ref": "hello world"}},
                  // Make sure that no optimization coalesces the above projection stage with the
                  // below one.
                  {$_internalInhibitOptimization: {}},
                  {$project: {x: "$$ROOT.$ref"}}
              ])
              .toArray(),
          [{_id: 0, x: "hello world"}]);

// Do a count (using $group) on a DBRef field.
assert.eq(coll.aggregate({$group: {_id: "$link.$db", count: {$sum: 1}}}).toArray(),
          [{_id: db.getName(), count: 1}]);

// Refer to a DBRef field in an accumulator.
assert.eq(coll.aggregate({$group: {_id: "$link.$db", count: {$sum: {$size: "$linkArray.$ref"}}}})
              .toArray(),
          [{_id: db.getName(), count: 2}]);

// Use $lookup with a DBRef.

// Equality match version.
const lookupEqualityPipeline = [{$lookup: {from: otherColl.getName(),
                                           localField: "link.$id",
                                           foreignField: "_id",
                                           as: "joinedField"}},
                        {$project: {link: 0, linkArray: 0}}];
assert.eq(coll.aggregate(lookupEqualityPipeline).toArray(),
          [{_id: 0, joinedField: [{_id: "id0", x: 1}]}]);

// Foreign pipeline.
const lookupSubPipePipeline = [{$lookup: {from: otherColl.getName(),
                                          let: {idsWanted: "$linkArray.$id"},
                                          pipeline: [{$match: {$expr: {$in: ["$_id", "$$idsWanted"]}}}],
                                          as: "joinedField"}},
                        {$project: {link: 0, linkArray: 0}}];
assert(anyEq(coll.aggregate(lookupSubPipePipeline).toArray(),
             [{_id: 0, joinedField: [{_id: "id0", x: 1}, {_id: "id1", x: 2}]}]));

(function testGraphLookup() {
    // $graphLookup using DBRef.
    const graphLookupColl = db.dbref_graph_lookup;
    graphLookupColl.drop();

    // id0 -> id1 -> id2 -> id0
    assert.commandWorked(graphLookupColl.insert(
        {_id: "id0", link: new DBRef(graphLookupColl.getName(), "id1", db.getName())}));
    assert.commandWorked(graphLookupColl.insert(
        {_id: "id1", link: new DBRef(graphLookupColl.getName(), "id2", db.getName())}));
    assert.commandWorked(graphLookupColl.insert(
        {_id: "id2", link: new DBRef(graphLookupColl.getName(), "id0", db.getName())}));

    // id3 -> id4
    assert.commandWorked(graphLookupColl.insert(
        {_id: "id3", link: new DBRef(graphLookupColl.getName(), "id4", db.getName())}));
    assert.commandWorked(graphLookupColl.insert({_id: "id4", link: null}));

    const graphLookupPipeline = [{
        $graphLookup: {
            from: graphLookupColl.getName(),
            startWith: "$link.$id",
            connectFromField: "link.$id",
            connectToField: "_id",
            as: "connectedDocuments"
        }
    },
                                 {$sort: {_id: 1}}];

    const res = graphLookupColl.aggregate(graphLookupPipeline).toArray();
    // id0, id1, and id2 are all connected.
    assert.eq(res[0].connectedDocuments.length, 3);
    assert.eq(res[1].connectedDocuments.length, 3);
    assert.eq(res[2].connectedDocuments.length, 3);

    // id3 is connected to id4.
    assert.eq(res[3].connectedDocuments.length, 1);

    // id4 is connected to nothing.
    assert.eq(res[4].connectedDocuments.length, 0);
    assert.eq(res.length, 5);
})();

// Distinct command for a dbref field.
assert.eq(coll.distinct("link.$ref"), [otherColl.getName()]);
assert.eq(coll.distinct("link.$id"), ["id0"]);
assert.eq(coll.distinct("link.$db"), [db.getName()]);

// $merge pipeline.
const thirdColl = db.dbref_in_expression_3;
thirdColl.drop();
assert.commandWorked(thirdColl.insert({_id: 0, a: 1}));
assert.commandWorked(coll.createIndex({"link.$ref": 1}, {unique: true}));

// Merge a document with a 'sentinel' field into the original collection using 'link.$ref' as the
// "on" field.
thirdColl
    .aggregate([
        {$project: {"link.$ref": otherColl.getName(), "link.$id": "id0", sentinel: "foo"}},
        {$merge: {into: coll.getName(), on: "link.$ref", whenMatched: "replace"}}
    ])
    .itcount();

// Check that the merge worked.
assert.eq(coll.find({sentinel: "foo"}).itcount(), 1);

// Merge using an update pipeline.
thirdColl
    .aggregate([
        {$project: {"link.$ref": otherColl.getName(), "link.$id": "id0", sentinel: "foo"}},
        {
            $merge: {
                into: coll.getName(),
                on: "link.$ref",
                whenMatched: [{
                    $project:
                        {"link.$ref": "otherRef", "link.$id": "otherId", "link.$db": "otherDB"}
                }],
                whenNotMatched: "discard"
            }
        }
    ])
    .itcount();
assert.eq(coll.find().toArray()[0], {_id: 0, link: new DBRef("otherRef", "otherId", "otherDB")});
})();
