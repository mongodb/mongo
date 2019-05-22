/**
 * Tests that $merge fails when the target collection is the aggregation collection.
 *
 * @tags: [assumes_unsharded_collection]
*/
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.name;
    coll.drop();

    const nDocs = 10;
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i}));
    }

    assertErrorCode(
        coll,
        [{$merge: {into: coll.getName(), whenMatched: "replaceWithNew", whenNotMatched: "insert"}}],
        51188);

    assertErrorCode(
        coll,
        [{$merge: {into: coll.getName(), whenMatched: "fail", whenNotMatched: "insert"}}],
        51188);
}());
