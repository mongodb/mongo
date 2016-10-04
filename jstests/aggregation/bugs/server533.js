// SERVER-533: Aggregation stage to randomly sample documents.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var coll = db.agg_sample;
    coll.drop();

    // Should return no results on a collection that doesn't exist. Should not crash.
    assert.eq(coll.aggregate([{$sample: {size: 10}}]).toArray(), []);

    var nItems = 3;
    for (var i = 0; i < nItems; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    [0, 1, nItems, nItems + 1].forEach(function(size) {
        var results = coll.aggregate([{$sample: {size: size}}]).toArray();
        assert.eq(results.length, Math.min(size, nItems));
    });

    // Multiple $sample stages are allowed.
    var results = coll.aggregate([{$sample: {size: nItems}}, {$sample: {size: 1}}]).toArray();
    assert.eq(results.length, 1);

    // Invalid options.
    assertErrorCode(coll, [{$sample: 'string'}], 28745);
    assertErrorCode(coll, [{$sample: {size: 'string'}}], 28746);
    assertErrorCode(coll, [{$sample: {size: -1}}], 28747);
    assertErrorCode(coll, [{$sample: {unknownOpt: true}}], 28748);
    assertErrorCode(coll, [{$sample: {/* no size */}}], 28749);
}());
