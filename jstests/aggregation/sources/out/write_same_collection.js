/**
 * Tests the behavior of $out when the target collection is the aggregation collection. Parsing
 * should fail if the aggregation has $out mode set to "replaceDocuments" or "insertDocuments",
 * but pass with the legacy syntax or when mode = "replaceCollection".
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

    assertErrorCode(coll, [{$out: {to: coll.getName(), mode: "replaceDocuments"}}], 50992);

    assertErrorCode(coll, [{$out: {to: coll.getName(), mode: "insertDocuments"}}], 50992);

    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: {to: coll.getName(), mode: "replaceCollection"}}],
        cursor: {},
    }));
}());
