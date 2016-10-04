load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'count',

        count: 100,
        countLt10: 10,
        countEq35: 2,
        countGte10: 90,

        load: function(writableCollection) {
            var bulk = writableCollection.initializeUnorderedBulkOp();

            for (var i = 0; i < this.countLt10; ++i) {
                bulk.insert({x: 5});
            }

            for (var i = 0; i < this.countEq35; ++i) {
                bulk.insert({x: 35});
            }

            for (var i = 0; i < this.countGte10 - this.countEq35; ++i) {
                bulk.insert({x: 70});
            }

            assert.writeOK(bulk.execute());
        },
        exec: function(readableCollection) {
            assert.eq(readableCollection.find({x: {$lt: 10}}).count(), this.countLt10);
            assert.eq(readableCollection.find({x: {$eq: 35}}).count(), this.countEq35);
            assert.eq(readableCollection.find({x: {$gte: 10}}).count(), this.countGte10);
            assert.eq(readableCollection.count(), this.count);
        }
    };
}());
