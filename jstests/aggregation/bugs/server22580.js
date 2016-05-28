// In SERVER-22580, the $substrCP expression was introduced. In this file, we test the error cases
// of this expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.substrCP;
    coll.drop();

    // Need an empty document for pipeline.
    coll.insert({});

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", 0, "a"]}}}],
                    34452,
                    "$substrCP" + " does not accept non-numeric types as a length.");

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", 0, NaN]}}}],
                    34453,
                    "$substrCP" + " does not accept non-integers as a length.");

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", "abc", 3]}}}],
                    34450,
                    "$substrCP does not accept non-numeric types as a starting index.");

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", 2.2, 3]}}}],
                    34451,
                    "$substrCP" + " does not accept non-integers as a starting index.");

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", -1, 3]}}}],
                    34455,
                    "$substrCP " + "does not accept negative integers as inputs.");

    assertErrorCode(coll,
                    [{$project: {substr: {$substrCP: ["abc", 1, -3]}}}],
                    34454,
                    "$substrCP " + "does not accept negative integers as inputs.");
}());
