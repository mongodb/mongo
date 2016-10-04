// In SERVER-20169, the $range expression was added to the aggregation framework. In this file, we
// test the behavior and error cases of this expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.range;
    coll.drop();

    // We need an input document to receive an output document.
    coll.insert({});

    var rangeObj = [1];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    28667,
                    "range requires two" + " or three arguments");

    rangeObj = ["a", 1];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34443,
                    "range requires a" + " numeric starting value");

    rangeObj = [1.1, 1];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34444,
                    "range requires an" + " integral starting value");

    rangeObj = [1, "a"];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34445,
                    "range requires a" + " numeric ending value");

    rangeObj = [1, 1.1];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34446,
                    "range requires an" + " integral ending value");

    rangeObj = [1, 3, "a"];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34447,
                    "range requires a" + " numeric step value");

    rangeObj = [1, 3, 1.1];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34448,
                    "range requires an" + " integral step value");

    rangeObj = [1, 3, 0];
    assertErrorCode(coll,
                    [{$project: {range: {$range: rangeObj}}}],
                    34449,
                    "range requires a" + " non-zero step value");
}());
