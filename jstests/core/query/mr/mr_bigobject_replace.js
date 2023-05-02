// Cannot implicitly shard accessed collections because of following errmsg: Cannot output to a
// non-sharded collection because sharded collection exists already.
// The test runs commands that are not allowed with security token: mapReduce.
// @tags: [
//   not_allowed_with_security_token,
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
//   requires_scripting,
// ]

/**
 * Test that the server returns an error response for map-reduce operations that attempt to insert a
 * document larger than 16MB as a result of the reduce() or finalize() functions and using the
 * "replace" action for the out collection.
 */
(function() {
"use strict";
function mapper() {
    // Emit multiple values to ensure that the reducer gets called.
    emit(this._id, 1);
    emit(this._id, 1);
}

function createBigDocument() {
    // Returns a document of the form { _id: ObjectId(...), value: '...' } with the specified
    // 'targetSize' in bytes.
    function makeDocWithSize(targetSize) {
        let doc = {_id: new ObjectId(), value: ''};

        let size = Object.bsonsize(doc);
        assert.gte(targetSize, size);

        // Set 'value' as a string with enough characters to make the whole document 'size'
        // bytes long.
        doc.value = new Array(targetSize - size + 1).join('x');
        assert.eq(targetSize, Object.bsonsize(doc));

        return doc;
    }

    let maxDocSize = 16 * 1024 * 1024;
    return makeDocWithSize(maxDocSize + 1).value;
}

function runTest(testOptions) {
    db.input.drop();
    db.mr_bigobject_replace.drop();

    // Insert a document so the mapper gets run.
    assert.commandWorked(db.input.insert({}));

    let res = db.runCommand(Object.extend({
        mapReduce: "input",
        map: mapper,
        out: {replace: "mr_bigobject_replace"},
    },
                                          testOptions));

    // In most cases we expect this to fail because it tries to insert a document that is too large,
    // or we see a particular error code which happens when the input is too large to reduce.
    //
    // In some cases we may see the javascript execution interrupted because it takes longer than
    // our default time limit, so we allow that possibility.
    const kCannotReduceLargeObjCode = 31392;
    assert.commandFailedWithCode(
        res,
        [ErrorCodes.BadValue, ErrorCodes.Interrupted, kCannotReduceLargeObjCode],
        "creating a document larger than 16MB didn't fail");
    // If we see 'BadValue', make sure the message indicates it's the kind of error we were
    // expecting.
    if (res.code === ErrorCodes.BadValue) {
        assert.lte(
            0,
            res.errmsg.indexOf("object to insert too large"),
            "map-reduce command failed for a reason other than inserting a large document: " +
                tojson(res));
    }
}

runTest({reduce: createBigDocument});
runTest({
    reduce: function() {
        return 1;
    },
    finalize: createBigDocument
});
})();
