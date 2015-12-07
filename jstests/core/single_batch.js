// Test the "single batch" semantics of negative limit.
(function() {
    'use strict';

    var coll = db.jstests_single_batch;
    coll.drop();

    // Approximately 1 MB.
    var padding = new Array(1024 * 1024).join("x");

    // Insert ~10 MB of data.
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({_id: i, padding: padding}));
    }

    // The limit is 8, but we should end up with fewer documents since 8 docs won't fit in a single
    // 4 MB batch.
    var numResults = coll.find().limit(-8).itcount();
    assert.lt(numResults, 8);
    assert.gt(numResults, 0);
})();
