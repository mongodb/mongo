/**
 * distinct_noindex.js
 *
 * Runs distinct on a non-indexed field and verifies the result.
 * The field contains non-unique values.
 * Each thread operates on the same collection.
 *
 */
export const $config = (function () {
    let data = {
        randRange: function randRange(low, high) {
            assert.gt(high, low);
            return low + Random.randInt(high - low + 1);
        },
        numDocs: 1000,
    };

    let states = (function () {
        function init(db, collName) {
            this.modulus = this.randRange(5, 15);

            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({i: i % this.modulus, tid: this.tid});
            }
            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.numDocs, res.nInserted);
        }

        function distinct(db, collName) {
            assert.eq(this.modulus, db[collName].distinct("i", {tid: this.tid}).length);
        }

        return {init: init, distinct: distinct};
    })();

    let transitions = {init: {distinct: 1}, distinct: {distinct: 1}};

    return {data: data, threadCount: 10, iterations: 20, states: states, transitions: transitions};
})();
