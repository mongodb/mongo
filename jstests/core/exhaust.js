// @tags: [requires_getmore]

(function() {
    'use strict';

    var c = db.exhaustColl;
    c.drop();

    const docCount = 4;
    for (var i = 0; i < docCount; i++) {
        assert.writeOK(c.insert({a: i}));
    }

    // Check that the query works without exhaust set
    assert.eq(c.find().batchSize(1).itcount(), docCount);

    // Now try to run the same query with exhaust
    try {
        assert.eq(c.find().batchSize(1).addOption(DBQuery.Option.exhaust).itcount(), docCount);
    } catch (e) {
        // The exhaust option is not valid against mongos, ensure that this query throws the right
        // code
        assert.eq(e.code, 18526);
    }

}());
