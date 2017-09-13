// SERVER-14670 introduced the $strLenBytes and $strLenCP aggregation expressions. In this file, we
// test the error cases for these expressions.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.substr;
    coll.drop();

    // Need an empty document for the pipeline.
    coll.insert({});

    assertErrorCode(coll,
                    [{$project: {strLen: {$strLenBytes: 1}}}],
                    34473,
                    "$strLenBytes requires a string argument.");

    assertErrorCode(coll,
                    [{$project: {strLen: {$strLenCP: 1}}}],
                    34471,
                    "$strLenCP requires a string argument.");
}());
