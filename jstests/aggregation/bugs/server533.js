// SERVER-533: Aggregation stage to randomly sample documents.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var coll = db.agg_sample;
    coll.drop();

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
    assertErrorCode(coll, [{$sample: 'string'}], 28739);
    assertErrorCode(coll, [{$sample: {size: 'string'}}], 28740);
    assertErrorCode(coll, [{$sample: {size: -1}}], 28741);
    assertErrorCode(coll, [{$sample: {unknownOpt: true}}], 28742);
    assertErrorCode(coll, [{$sample: {/* no size */}}], 28743);
}());
