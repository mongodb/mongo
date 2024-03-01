/**
 * Some more tests $and/$or being nested in various ways.
 */

const coll = db.jstests_and_or_index_sort;
coll.drop();

function runWithDifferentIndexes(keyPatternsList, testFunc) {
    for (const keyPatterns of keyPatternsList) {
        for (const keyPattern of keyPatterns) {
            assert.commandWorked(coll.createIndex(keyPattern));
        }
        testFunc();
        assert.commandWorked(coll.dropIndexes());
    }
}

assert.commandWorked(coll.insert([
    {_id: 1, a: 8, b: 3, c: 4, d: 0},
    {_id: 2, a: 1, b: 5, c: 9, d: 1},
    {_id: 3, a: 6, b: 7, c: 2, d: 1},
    {_id: 4, a: 4, b: 8, c: 3, d: 0},
    {_id: 5, a: 9, b: 1, c: 5, d: 1},
    {_id: 6, a: 2, b: 6, c: 7, d: 0},
    {_id: 7, a: 3, b: 4, c: 8, d: 0},
    {_id: 8, a: 5, b: 9, c: 1, d: 0},
    {_id: 9, a: 7, b: 2, c: 6, d: 1},
    {_id: 10, b: 3, c: 4.5, d: 0},
    {_id: 11, a: 8, b: 3.5, d: 0},
    {_id: 12, a: 9, c: 5.5, d: 1},
    {_id: 13, a: 9, b: 1.5, d: 1}
]));

runWithDifferentIndexes(
    [[], [{a: 1}, {b: 1, c: 1}], [{a: 1, c: 1}, {b: 1}], [{a: 1, c: 1}, {b: 1, c: 1}]], () => {
        assert.docEq([{_id: 6, a: 2, b: 6, c: 7, d: 0}, {_id: 2, a: 1, b: 5, c: 9, d: 1}],
                     coll.find({a: {$lt: 3}}).sort({c: 1, a: 1}).toArray());

        assert.docEq(
            [
                {_id: 13, a: 9, b: 1.5, d: 1},
                {_id: 5, a: 9, b: 1, c: 5, d: 1},
                {_id: 12, a: 9, c: 5.5, d: 1}
            ],
            coll.find({$or: [{a: {$gt: 8}}, {b: {$lt: 2}}]}).sort({c: 1}).toArray());

        assert.docEq(
            [
                {_id: 13, a: 9, b: 1.5, d: 1},
                {_id: 1, a: 8, b: 3, c: 4, d: 0},
                {_id: 10, b: 3, c: 4.5, d: 0},
                {_id: 5, a: 9, b: 1, c: 5, d: 1},
                {_id: 12, a: 9, c: 5.5, d: 1},
                {_id: 9, a: 7, b: 2, c: 6, d: 1}
            ],
            coll.find(
                    {$or: [{a: {$gt: 8}}, {$and: [{b: {$lt: 5}}, {$or: [{c: {$lt: 5}}, {d: 1}]}]}]})
                .sort({c: 1})
                .toArray());

        assert.docEq(
            [
                {_id: 10, b: 3, c: 4.5, d: 0},
                {_id: 9, a: 7, b: 2, c: 6, d: 1},
                {_id: 1, a: 8, b: 3, c: 4, d: 0},
                {_id: 11, a: 8, b: 3.5, d: 0},
                {_id: 12, a: 9, c: 5.5, d: 1},
                {_id: 5, a: 9, b: 1, c: 5, d: 1},
                {_id: 13, a: 9, b: 1.5, d: 1}
            ],
            coll.find({$or: [{a: {$gt: 6}}, {b: {$lt: 4}}]}).sort({a: 1, b: 1}).toArray());

        assert.sameMembers(coll.find({$or: [{a: {$gt: 6}}, {b: {$lt: 4}}]}).toArray(), [
            {_id: 9, a: 7, b: 2, c: 6, d: 1},
            {_id: 11, a: 8, b: 3.5, d: 0},
            {_id: 1, a: 8, b: 3, c: 4, d: 0},
            {_id: 13, a: 9, b: 1.5, d: 1},
            {_id: 5, a: 9, b: 1, c: 5, d: 1},
            {_id: 12, a: 9, c: 5.5, d: 1},
            {_id: 10, b: 3, c: 4.5, d: 0}
        ]);

        assert.sameMembers(
            coll.find(
                    {$or: [{a: {$gt: 8}}, {$and: [{b: {$lt: 5}}, {$or: [{c: {$lt: 5}}, {d: 1}]}]}]})
                .toArray(),
            [
                {_id: 5, a: 9, b: 1, c: 5, d: 1},
                {_id: 13, a: 9, b: 1.5, d: 1},
                {_id: 9, a: 7, b: 2, c: 6, d: 1},
                {_id: 1, a: 8, b: 3, c: 4, d: 0},
                {_id: 10, b: 3, c: 4.5, d: 0},
                {_id: 12, a: 9, c: 5.5, d: 1}
            ]);

        assert.sameMembers(
            coll.find({
                    $or: [{$and: [{a: {$gt: 5}}, {c: {$gt: 2}}]}, {$and: [{b: {$lt: 5}}, {d: 1}]}]
                })
                .toArray(),
            [
                {_id: 9, a: 7, b: 2, c: 6, d: 1},
                {_id: 1, a: 8, b: 3, c: 4, d: 0},
                {_id: 5, a: 9, b: 1, c: 5, d: 1},
                {_id: 12, a: 9, c: 5.5, d: 1},
                {_id: 13, a: 9, b: 1.5, d: 1}
            ]);

        // Depending on what indexes exist, the query plans for the queries below might or might
        // not involve doing a covered projection. If a covered projection is involved, each object
        // returned by the query will always have a "c" field even if the original underlying
        // document from the collection doesn't have a "c" field. If a covered projection is NOT
        // involved, each object returned by the query will only have a "c" field if the original
        // underlying document has a "c" field. In order to make it easier to compare these queries'
        // outputs against expected outputs, we perform a transformation on the data returned by
        // these queries.

        assert.eq(coll.find({a: {$gt: 6}}, {c: 1, _id: 0}).sort({c: 1}).toArray().map(obj => {
            if (!obj.hasOwnProperty('c')) {
                obj.c = null;
            }
            return obj;
        }),
                  [{c: null}, {c: null}, {c: 4}, {c: 5}, {c: 5.5}, {c: 6}]);

        assert.docEq([{c: null}, {c: null}, {c: 4}, {c: 4.5}, {c: 5}, {c: 5.5}, {c: 6}],
                     coll.find({$or: [{a: {$gt: 6}}, {b: {$lt: 4}}]}, {c: 1, _id: 0})
                         .sort({c: 1})
                         .toArray()
                         .map(obj => {
                             if (!obj.hasOwnProperty('c')) {
                                 obj.c = null;
                             }
                             return obj;
                         }));
    });
