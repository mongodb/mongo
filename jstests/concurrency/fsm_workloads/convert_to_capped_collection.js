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
 * @tags: [requires_collstats, requires_capped]
 */

var $config = (function() {
    load("jstests/libs/feature_flag_util.js");

    // TODO: This workload may fail if an iteration multiplier is specified.
    var data = {prefix: 'convert_to_capped_collection'};

    var states = (function() {
        function uniqueCollectionName(prefix, tid) {
            return prefix + '_' + tid;
        }

        function isMultiple256(num) {
            return num % 256 === 0;
        }

        function init(db, collName) {
            this.threadCollName = uniqueCollectionName(this.prefix, this.tid);

            var bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (var i = 0; i < (this.tid + 1) * 200; i++) {
                bulk.insert({i: i, rand: Random.rand()});
            }

            var res = bulk.execute();
            assertAlways.commandWorked(res);
            assertAlways.eq((this.tid + 1) * 200, res.nInserted);

            assertWhenOwnDB(!db[this.threadCollName].isCapped());
            assertWhenOwnDB.commandWorked(db[this.threadCollName].convertToCapped(this.size));
            assertWhenOwnDB(db[this.threadCollName].isCapped());
            if (!FeatureFlagUtil.isEnabled(db, "CappedCollectionsRelaxedSize")) {
                assertWhenOwnDB(isMultiple256(db[this.threadCollName].stats().maxSize));
            }
        }

        function convertToCapped(db, collName) {
            // divide size by 1.5 so that the resulting size
            // is not a multiple of 256
            this.size /= 1.5;

            assertWhenOwnDB.commandWorked(db[this.threadCollName].convertToCapped(this.size));
            assertWhenOwnDB(db[this.threadCollName].isCapped());
            if (!FeatureFlagUtil.isEnabled(db, "CappedCollectionsRelaxedSize")) {
                assertWhenOwnDB(isMultiple256(db[this.threadCollName].stats().maxSize));
            }

            // only the _id index should remain after running convertToCapped
            var indexKeys = db[this.threadCollName].getIndexKeys();
            assertWhenOwnDB.eq(1, indexKeys.length);
            assertWhenOwnDB(function() {
                assertWhenOwnDB.docEq({_id: 1}, indexKeys[0]);
            });
        }

        return {init: init, convertToCapped: convertToCapped};
    })();

    var transitions = {init: {convertToCapped: 1}, convertToCapped: {convertToCapped: 1}};

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
