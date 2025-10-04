import {runReadOnlyTest} from "jstests/readonly/lib/read_only_test.js";

runReadOnlyTest(
    (function () {
        return {
            name: "find",
            load: function (writableCollection) {
                for (let i = 0; i < 10; ++i) {
                    assert.commandWorked(writableCollection.insert({x: i, y: 2 * i}));
                }
            },
            exec: function (readableCollection) {
                let res = readableCollection.findOne({x: 3});
                assert.neq(res, null);
                assert.eq(res.y, 6);

                assert.eq(readableCollection.find({x: {$gt: 3, $lte: 6}}).count(), 3);
                assert.eq(readableCollection.find({y: {$lte: -1}}).count(), 0);
                assert.eq(readableCollection.find({$or: [{x: {$lte: 2}}, {y: {$gte: 16}}]}).count(), 5);
            },
        };
    })(),
);
