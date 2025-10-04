/**
 * convert_to_capped_collection.js
 *
 * Creates a non-capped collection. Converts it to a
 * capped collection. After each iteration, truncates the
 * collection, ensuring that the storage size of the
 * collection is still a multiple of 256.
 *
 * MongoDB raises the storage size of a capped collection
 * to an integer multiple of 256.
 *
 * @tags: [
 *   # convertToCapped can't be run on sharded collections.
 *   assumes_unsharded_collection,
 *   # convertToCapped requires a global lock and any background operations on the database causes
 *   # it to fail due to not finishing quickly enough.
 *   incompatible_with_concurrency_simultaneous,
 *   requires_collstats,
 *   requires_capped
 * ]
 */
export const $config = (function () {
    // TODO: This workload may fail if an iteration multiplier is specified.
    let data = {prefix: "convert_to_capped_collection"};

    let states = (function () {
        function uniqueCollectionName(prefix, tid) {
            return prefix + "_" + tid;
        }

        function init(db, collName) {
            this.threadCollName = uniqueCollectionName(this.prefix, this.tid);

            let bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (let i = 0; i < (this.tid + 1) * 200; i++) {
                bulk.insert({i: i, rand: Random.rand()});
            }

            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq((this.tid + 1) * 200, res.nInserted);

            assert(!db[this.threadCollName].isCapped());
            assert.commandWorked(db[this.threadCollName].convertToCapped(this.size));
            assert(db[this.threadCollName].isCapped());
        }

        function convertToCapped(db, collName) {
            // divide size by 1.5 so that the resulting size
            // is not a multiple of 256
            this.size /= 1.5;

            assert.commandWorked(db[this.threadCollName].convertToCapped(this.size));
            assert(db[this.threadCollName].isCapped());

            // only the _id index should remain after running convertToCapped
            let indexKeys = db[this.threadCollName].getIndexKeys();
            assert.eq(1, indexKeys.length);
            assert.docEq({_id: 1}, indexKeys[0]);
        }

        return {init: init, convertToCapped: convertToCapped};
    })();

    let transitions = {init: {convertToCapped: 1}, convertToCapped: {convertToCapped: 1}};

    function setup(db, collName, cluster) {
        // Initial size should not be a power of 256.
        this.size = Math.pow(2, this.iterations + 5) + 1;
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
