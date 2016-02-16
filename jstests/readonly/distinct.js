load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'find',

        colors: ['blue', 'green', 'orange', 'white'],
        nums: [1, 2, 3, 4, 5, 6],

        load: function(writableCollection) {
            var N = 1000;

            this.colors.sort();
            this.nums.sort();

            var bulk = writableCollection.initializeUnorderedBulkOp();

            for (var [color, num] of zip2(cycleN(this.colors, N), cycleN(this.nums, N))) {
                bulk.insert({color, num});
            }
            assert.writeOK(bulk.execute());
        },
        exec: function(readableCollection) {
            var distinctColors = readableCollection.distinct('color');
            var distinctNums = readableCollection.distinct('num');

            distinctColors.sort();
            distinctNums.sort();

            assert.eq(distinctColors.length, this.colors.length);
            for (var i = 0; i < distinctColors; ++i) {
                assert.eq(distinctColors[i], this.colors[i]);
            }

            assert.eq(distinctNums.length, this.nums.length);
            for (var i = 0; i < distinctNums; ++i) {
                assert.eq(distinctNums[i], this.nums[i]);
            }
        }
    };
}());
