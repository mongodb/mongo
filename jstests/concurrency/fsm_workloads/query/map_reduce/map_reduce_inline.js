/**
 * map_reduce_inline.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field in memory.
 *
 * Used as the base workload for the other map-reduce workloads.
 * @tags: [
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency,
 *   # Use mapReduce.
 *   requires_scripting,
 *   # Disabled because MapReduce can lose cursors if the primary goes down during the operation.
 *   does_not_support_stepdowns,
 *   # TODO (SERVER-95170): Re-enable this test in txn suites.
 *   does_not_support_transactions,
 *   # TODO (SERVER-91002): server side javascript execution is deprecated, and the balancer is not
 *   # compatible with it, once the incompatibility is taken care off we can re-enable this test
 *   assumes_balancer_off
 * ]
 */
export const $config = (function () {
    function mapper() {
        if (this.hasOwnProperty("key") && this.hasOwnProperty("value")) {
            let obj = {};
            obj[this.value] = 1;
            emit(this.key, obj);
        }
    }

    function reducer(key, values) {
        let res = {};

        values.forEach(function (obj) {
            Object.keys(obj).forEach(function (value) {
                if (!res.hasOwnProperty(value)) {
                    res[value] = 0;
                }
                res[value] += obj[value];
            });
        });

        return res;
    }

    function finalizer(key, reducedValue) {
        return reducedValue;
    }

    let data = {numDocs: 2000, mapper: mapper, reducer: reducer, finalizer: finalizer};

    let states = (function () {
        function init(db, collName) {
            // no-op
            // other workloads that extend this workload use this method
        }

        function mapReduce(db, collName) {
            let options = {finalize: this.finalizer, out: {inline: 1}};

            let res = db[collName].mapReduce(this.mapper, this.reducer, options);
            assert.commandWorked(res);
        }

        return {init: init, mapReduce: mapReduce};
    })();

    let transitions = {init: {mapReduce: 1}, mapReduce: {mapReduce: 1}};

    function setup(db, collName, cluster) {
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            // TODO: this actually does assume that there are no unique indexes
            bulk.insert({
                _id: i,
                key: Random.randInt(this.numDocs / 100),
                value: Random.randInt(this.numDocs / 10),
            });
        }

        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
    }

    return {
        threadCount: 5,
        iterations: 10,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
