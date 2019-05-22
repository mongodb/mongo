// Tests for the $merge stage with whenMatched: "replace" and whenNotMatched: "insert".
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
    load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

    const coll = db.merge_replace_insert;
    const outColl = db.merge_replace_insert_out;
    coll.drop();
    outColl.drop();

    const nDocs = 10;
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i}));
    }

    // Test that a $merge with whenMatched: "replace" and whenNotMatched: "insert" mode will
    // default the "on" fields to "_id".
    coll.aggregate(
        [{$merge: {into: outColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}]);
    assert.eq(nDocs, outColl.find().itcount());

    // Test that $merge will update existing documents that match the "on" fields.
    const nDocsReplaced = 5;
    coll.aggregate([
        {$project: {_id: {$mod: ["$_id", nDocsReplaced]}}},
        {
          $merge: {
              into: outColl.getName(),
              whenMatched: "replace",
              whenNotMatched: "insert",
              on: "_id"
          }
        }
    ]);
    assert.eq(nDocsReplaced, outColl.find({a: {$exists: false}}).itcount());

    // Test $merge with a dotted path "on" fields.
    coll.drop();
    outColl.drop();
    assert.commandWorked(coll.insert([{_id: 0, a: {b: 1}}, {_id: 1, a: {b: 1}, c: 1}]));
    assert.commandWorked(outColl.createIndex({"a.b": 1, _id: 1}, {unique: true}));
    coll.aggregate([
        {$addFields: {_id: 0}},
        {
          $merge: {
              into: outColl.getName(),
              whenMatched: "replace",
              whenNotMatched: "insert",
              on: ["_id", "a.b"]
          }
        }
    ]);
    assert.eq([{_id: 0, a: {b: 1}, c: 1}], outColl.find().toArray());

    // Test that $merge will automatically generate a missing "_id" for the "on" field.
    coll.drop();
    outColl.drop();
    assert.commandWorked(coll.insert({field: "will be removed"}));
    assert.doesNotThrow(() => coll.aggregate([
        {$replaceRoot: {newRoot: {}}},
        {
          $merge: {
              into: outColl.getName(),
              whenMatched: "replace",
              whenNotMatched: "insert",
          }
        }
    ]));
    assert.eq(1, outColl.find({field: {$exists: false}}).itcount());

    // Test that $merge will automatically generate a missing "_id", and the aggregation succeeds
    // with multiple "on" fields.
    outColl.drop();
    assert.commandWorked(outColl.createIndex({name: -1, _id: 1}, {unique: true, sparse: true}));
    assert.doesNotThrow(() => coll.aggregate([
        {$replaceRoot: {newRoot: {name: "jungsoo"}}},
        {
          $merge: {
              into: outColl.getName(),
              whenMatched: "replace",
              whenNotMatched: "insert",
              on: ["_id", "name"]
          }
        }
    ]));
    assert.eq(1, outColl.find().itcount());

    // Test that we will not attempt to modify the _id of an existing document if the _id is
    // projected away but the "on" field does not involve _id.
    coll.drop();
    assert.commandWorked(coll.insert({name: "kyle"}));
    assert.commandWorked(coll.insert({name: "nick"}));
    outColl.drop();
    assert.commandWorked(outColl.createIndex({name: 1}, {unique: true}));
    assert.commandWorked(outColl.insert({_id: "must be unchanged", name: "kyle"}));
    assert.doesNotThrow(() => coll.aggregate([
        {$project: {_id: 0}},
        {$addFields: {newField: 1}},
        {
          $merge: {
              into: outColl.getName(),
              whenMatched: "replace",
              whenNotMatched: "insert",
              on: "name"
          }
        }
    ]));
    const outResult = outColl.find().sort({name: 1}).toArray();
    const errmsgFn = () => tojson(outResult);
    assert.eq(2, outResult.length, errmsgFn);
    assert.docEq({_id: "must be unchanged", name: "kyle", newField: 1}, outResult[0], errmsgFn);
    assert.eq("nick", outResult[1].name, errmsgFn);
    assert.eq(1, outResult[1].newField, errmsgFn);
    assert.neq(null, outResult[1]._id, errmsgFn);

    // Test that $merge with a missing non-id "on" field fails.
    outColl.drop();
    assert.commandWorked(outColl.createIndex({missing: 1}, {unique: true}));
    assertErrorCode(
        coll,
        [{
           $merge: {
               into: outColl.getName(),
               whenMatched: "replace",
               whenNotMatched: "insert",
               on: "missing"
           }
        }],
        51132  // This attempt should fail because there's no field 'missing' in the document.
        );

    // Test that a replace fails to insert a document if it violates a unique index constraint. In
    // this example, $merge will attempt to insert multiple documents with {a: 0} which is not
    // allowed with the unique index on {a: 1}.
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}]));

    outColl.drop();
    assert.commandWorked(outColl.createIndex({a: 1}, {unique: true}));
    assertErrorCode(
        coll,
        [
          {$addFields: {a: 0}},
          {$merge: {into: outColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
        ],
        ErrorCodes.DuplicateKey);

    // Test that $merge fails if the "on" fields contains an array.
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: [1, 2]}));
    assert.commandWorked(outColl.createIndex({"a.b": 1, _id: 1}, {unique: true}));
    assertErrorCode(coll,
                    [
                      {$addFields: {_id: 0}},
                      {
                        $merge: {
                            into: outColl.getName(),
                            whenMatched: "replace",
                            whenNotMatched: "insert",
                            on: ["_id", "a.b"]
                        }
                      }
                    ],
                    51132);

    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: [{b: 1}]}));
    assertErrorCode(coll,
                    [
                      {$addFields: {_id: 0}},
                      {
                        $merge: {
                            into: outColl.getName(),
                            whenMatched: "replace",
                            whenNotMatched: "insert",
                            on: ["_id", "a.b"]
                        }
                      }
                    ],
                    51132);

    // Tests for $merge to a database that differs from the aggregation database.
    const foreignDb = db.getSiblingDB("merge_replace_insert_foreign");
    const foreignTargetColl = foreignDb.out;
    const pipelineDifferentOutputDb = [{
        $merge: {
            into: {
                db: foreignDb.getName(),
                coll: foreignTargetColl.getName(),
            },
            whenMatched: "replace",
            whenNotMatched: "insert",
        }
    }];

    coll.drop();
    assert.commandWorked(coll.insert({_id: 0}));
    foreignDb.dropDatabase();

    if (!FixtureHelpers.isMongos(db)) {
        // Test that $merge implicitly creates a new database when the output collection's database
        // doesn't exist.
        coll.aggregate(pipelineDifferentOutputDb);
        assert.eq(foreignTargetColl.find().itcount(), 1);
    } else {
        // Implicit database creation is prohibited in a cluster.
        let error = assert.throws(() => coll.aggregate(pipelineDifferentOutputDb));
        assert.commandFailedWithCode(error, ErrorCodes.NamespaceNotFound);

        // Force a creation of the database and collection, then fall through the test below.
        assert.commandWorked(foreignTargetColl.insert({_id: 0}));
    }

    // Insert a new document into the source collection, then test that running the same
    // aggregation will replace existing documents in the foreign output collection when
    // applicable.
    coll.drop();
    const newDocuments = [{_id: 0, newField: 1}, {_id: 1}];
    assert.commandWorked(coll.insert(newDocuments));
    coll.aggregate(pipelineDifferentOutputDb);
    assert.eq(foreignTargetColl.find().sort({_id: 1}).toArray(), newDocuments);
}());
