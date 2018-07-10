// Tests for the $out stage with mode set to "replaceDocuments".
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.replace_docs;
    const outColl = db.replace_docs_out;
    coll.drop();
    outColl.drop();

    const nDocs = 10;
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i}));
    }

    // Test that a $out with 'replaceDocuments' mode will default the unique key to "_id".
    coll.aggregate([{$out: {to: outColl.getName(), mode: "replaceDocuments"}}]);
    assert.eq(nDocs, outColl.find().itcount());

    // Test that 'replaceDocuments' mode will update existing documents that match the unique key.
    const nDocsReplaced = 5;
    coll.aggregate([
        {$project: {_id: {$mod: ["$_id", nDocsReplaced]}}},
        {$out: {to: outColl.getName(), mode: "replaceDocuments", uniqueKey: {_id: 1}}}
    ]);
    assert.eq(nDocsReplaced, outColl.find({a: {$exists: false}}).itcount());

    // Test 'replaceDocuments' mode with a dotted path unique key.
    coll.drop();
    outColl.drop();
    assert.commandWorked(coll.insert({_id: 0, a: {b: 1}}));
    assert.commandWorked(coll.insert({_id: 1, a: {b: 1}, c: 1}));
    coll.aggregate([
        {$addFields: {_id: 0}},
        {$out: {to: outColl.getName(), mode: "replaceDocuments", uniqueKey: {_id: 1, "a.b": 1}}}
    ]);
    assert.eq([{_id: 0, a: {b: 1}, c: 1}], outColl.find().toArray());

    // TODO SERVER-36100: 'replaceDocuments' mode should allow a missing "_id" unique key.
    assertErrorCode(coll,
                    [
                      {$project: {_id: 0}},
                      {
                        $out: {
                            to: outColl.getName(),
                            mode: "replaceDocuments",
                        }
                      }
                    ],
                    50905);

    // Test that 'replaceDocuments' mode with a missing non-id unique key fails.
    assertErrorCode(
        coll,
        [{$out: {to: outColl.getName(), mode: "replaceDocuments", uniqueKey: {missing: 1}}}],
        50905);

    // Test that a replace fails to insert a document if it violates a unique index constraint. In
    // this example, $out will attempt to insert multiple documents with {a: 0} which is not allowed
    // with the unique index on {a: 1}.
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}]));

    outColl.drop();
    assert.commandWorked(outColl.createIndex({a: 1}, {unique: true}));
    assertErrorCode(
        coll,
        [{$addFields: {a: 0}}, {$out: {to: outColl.getName(), mode: "replaceDocuments"}}],
        ErrorCodes.DuplicateKey);

    // Test that $out fails if the unique key contains an array.
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: [1, 2]}));
    assertErrorCode(
        coll,
        [
          {$addFields: {_id: 0}},
          {$out: {to: outColl.getName(), mode: "replaceDocuments", uniqueKey: {_id: 1, "a.b": 1}}}
        ],
        50905);

    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: [{b: 1}]}));
    assertErrorCode(
        coll,
        [
          {$addFields: {_id: 0}},
          {$out: {to: outColl.getName(), mode: "replaceDocuments", uniqueKey: {_id: 1, "a.b": 1}}}
        ],
        50905);
}());
