'use strict';

/**
 * map_reduce_inline.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field in memory.
 *
 * Used as the base workload for the other map-reduce workloads.
 */
var $config = (function() {

    function mapper() {
        if (this.hasOwnProperty('key') && this.hasOwnProperty('value')) {
            var obj = {};
            obj[this.value] = 1;
            emit(this.key, obj);
        }
    }

    function reducer(key, values) {
        var res = {};

        values.forEach(function(obj) {
            Object.keys(obj).forEach(function(value) {
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

    var data = {numDocs: 2000, mapper: mapper, reducer: reducer, finalizer: finalizer};

    var states = (function() {

        function init(db, collName) {
            // no-op
            // other workloads that extend this workload use this method
        }

        function mapReduce(db, collName) {
            var options = {finalize: this.finalizer, out: {inline: 1}};

            var res = db[collName].mapReduce(this.mapper, this.reducer, options);
            assertAlways.commandWorked(res);
        }

        return {init: init, mapReduce: mapReduce};

    })();

    var transitions = {init: {mapReduce: 1}, mapReduce: {mapReduce: 1}};

    function makeDoc(keyLimit, valueLimit) {
        return {
            _id: new ObjectId(),
            key: Random.randInt(keyLimit),
            value: Random.randInt(valueLimit)
        };
    }

    function setup(db, collName, cluster) {
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            // TODO: this actually does assume that there are no unique indexes
            var doc = makeDoc(this.numDocs / 100, this.numDocs / 10);
            bulk.insert(doc);
        }

        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.numDocs, res.nInserted);
    }

    return {
        threadCount: 5,
        iterations: 10,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
