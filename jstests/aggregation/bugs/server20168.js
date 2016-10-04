// SERVER-20168: Add option to $unwind to output a null result for empty arrays.
(function() {
    "use strict";

    var coll = db.server20168;
    coll.drop();

    // Should return no results on a non-existent collection.
    var results = coll.aggregate([{$unwind: {path: "$x"}}]).toArray();
    assert.eq(0, results.length, "$unwind returned the wrong number of results");

    /**
     * Asserts that with the input 'inputDoc', an $unwind stage on 'unwindPath' should produce no
     * results if preserveNullAndEmptyArrays is not specified, and produces one result, equal to
     * 'outputDoc', if it is specified.
     */
    function testPreserveNullAndEmptyArraysParam(inputDoc, unwindPath, outputDoc) {
        coll.drop();
        assert.writeOK(coll.insert(inputDoc));

        // If preserveNullAndEmptyArrays is passed, we should get an output document.
        var preservedResults =
            coll.aggregate([{$unwind: {path: unwindPath, preserveNullAndEmptyArrays: true}}])
                .toArray();
        assert.eq(1, preservedResults.length, "$unwind returned the wrong number of results");
        assert.eq(preservedResults[0],
                  outputDoc,
                  "Unexpected result for an $unwind with preserveNullAndEmptyArrays " +
                      "(input was " + tojson(inputDoc) + ")");

        // If not, we should get no outputs.
        var defaultResults = coll.aggregate([{$unwind: {path: unwindPath}}]).toArray();
        assert.eq(0, defaultResults.length, "$unwind returned the wrong number of results");
    }

    testPreserveNullAndEmptyArraysParam({_id: 0}, "$x", {_id: 0});
    testPreserveNullAndEmptyArraysParam({_id: 0, x: null}, "$x", {_id: 0, x: null});
    testPreserveNullAndEmptyArraysParam({_id: 0, x: []}, "$x", {_id: 0});
}());
