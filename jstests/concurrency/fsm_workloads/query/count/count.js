/**
 * count.js
 *
 * Base workload for count.
 * Runs count on a non-indexed field and verifies that the count
 * is correct.
 * Each thread picks a random 'modulus' in range [5, 10]
 * and a random 'countPerNum' in range [50, 100]
 * and then inserts 'modulus * countPerNum' documents. [250, 1000]
 * All threads insert into the same collection.
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        randRange: function randRange(low, high) {
            // return random number in range [low, high]
            assert.gt(high, low);
            return low + Random.randInt(high - low + 1);
        },
        getNumDocs: function getNumDocs() {
            return this.modulus * this.countPerNum;
        },
        getCount: function getCount(db, predicate) {
            let query = Object.extend({tid: this.tid}, predicate);
            return db[this.threadCollName].count(query);
        },
    };

    let states = (function () {
        function init(db, collName) {
            this.modulus = this.randRange(5, 10);
            this.countPerNum = this.randRange(50, 100);

            // workloads that extend this one might have set 'this.threadCollName'
            this.threadCollName = this.threadCollName || collName;

            let bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.getNumDocs(); ++i) {
                bulk.insert({i: i % this.modulus, tid: this.tid});
            }
            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.getNumDocs(), res.nInserted);
        }

        function count(db, collName) {
            if (!isMongos(db)) {
                // SERVER-33753: count() without a predicate can be wrong on sharded clusters.
                assert.eq(this.getCount(db), this.getNumDocs());
            }

            let num = Random.randInt(this.modulus);
            assert.eq(this.getCount(db, {i: num}), this.countPerNum);
        }

        return {init: init, count: count};
    })();

    let transitions = {init: {count: 1}, count: {count: 1}};

    return {data: data, threadCount: 10, iterations: 20, states: states, transitions: transitions};
})();
