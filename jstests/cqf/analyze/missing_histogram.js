/**
 * This test verifies that we gracefully handle the case where we do not have statistics or a
 * histogram available for a given path. It also tests empty collections are handled appropriately.
 */
(function() {
"use strict";

load('jstests/libs/ce_stats_utils.js');

runHistogramsTest(function testEmptyAndMissingHistograms() {
    const emptyColl = db.missing_histogram_empty;
    emptyColl.drop();
    assert.commandWorked(emptyColl.createIndex({"notAField": 1}));
    assert.commandWorked(emptyColl.createIndex({"not.a.field": 1}));

    createAndValidateHistogram({
        coll: emptyColl,
        expectedHistogram: {
            _id: "notAField",
        },
        empty: true,
    });

    const noHistogramColl = db.no_histogram;
    noHistogramColl.drop();
    noHistogramColl.insert({_id: 0, notAField: 1});
    assert.commandWorked(noHistogramColl.createIndex({"notAField": 1}));

    // Ensure we have no histogram.
    assert.eq(db.system.statistics[noHistogramColl.getName()].count(), 0);

    forceCE("histogram");

    // We actually use heuristics because we don't generate histograms for empty collections;
    // however, our estimate should still be 0, because the collection is empty.
    verifyCEForMatch(
        {coll: emptyColl, predicate: {notAField: 1}, expected: [], hint: {notAField: 1}});

    // We should use heuristics in this case since we have no histogram. Our estimate happens to be
    // correct because we only have one document.
    verifyCEForMatch({
        coll: noHistogramColl,
        predicate: {notAField: 1},
        expected: [{_id: 0, notAField: 1}],
        hint: {notAField: 1},
    });
});
}());
