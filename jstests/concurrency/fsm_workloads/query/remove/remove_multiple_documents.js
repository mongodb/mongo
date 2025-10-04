/**
 * remove_multiple_documents.js
 *
 * Each thread first inserts 200 documents, each containing the thread id and
 * a random float. Then on each iteration, each thread repeatedly removes some
 * of the documents it inserted.
 *
 * When the balancer is enabled, the nRemoved result may be inaccurate as
 * a chunk migration may be active, causing the count function to assert.
 *
 * @tags: [assumes_balancer_off, requires_getmore]
 */
export const $config = (function () {
    let states = {
        init: function init(db, collName) {
            this.numDocs = 200;
            for (let i = 0; i < this.numDocs; ++i) {
                db[collName].insert({tid: this.tid, rand: Random.rand()});
            }
        },

        remove: function remove(db, collName) {
            // choose a random interval to remove documents from
            let low = Random.rand();
            let high = low + 0.05 * Random.rand();

            let res = db[collName].remove({tid: this.tid, rand: {$gte: low, $lte: high}});
            assert.gte(res.nRemoved, 0);
            assert.lte(res.nRemoved, this.numDocs);
            this.numDocs -= res.nRemoved;
        },

        count: function count(db, collName) {
            let numDocs = db[collName].find({tid: this.tid}).itcount();
            assert.eq(this.numDocs, numDocs);
        },
    };

    let transitions = {init: {count: 1}, count: {remove: 1}, remove: {remove: 0.825, count: 0.125}};

    return {threadCount: 10, iterations: 20, states: states, transitions: transitions};
})();
