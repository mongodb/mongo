load("jstests/readonly/lib/read_only_test.js");

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'get_more',
        count: 3000,
        load: function(writableCollection) {
            // Insert enough data that we need multiple batches to get it.
            var n = this.count;
            var bulkSize = 500;

            for (var i = 0; i < n / bulkSize; ++i) {
                var bulk = writableCollection.initializeUnorderedBulkOp();
                for (var j = 0; j < bulkSize; ++j) {
                    var idx = i * bulkSize + j;
                    bulk.insert({x: idx, y: idx + 1});
                }
                assert.writeOK(bulk.execute());
            }
            assert.eq(writableCollection.count(), this.count);
        },
        exec: function(readableCollection) {
            var cursor = readableCollection.find();
            var count = 0;
            while (cursor.hasNext()) {
                ++count;
                var doc = cursor.next();
                assert.eq(doc.y, doc.x + 1);
            }
            assert.eq(count, this.count);
        }
    };
}());
