'use strict';

/**
 * agg_base.js
 *
 * Base workload for aggregation. Inserts a bunch of documents in its setup,
 * then each thread does an aggregation with an empty $match.
 */
var $config = (function() {

    var data = {
        numDocs: 1000,
        // Use 12KB documents by default. This number is useful because 12,000 documents each of
        // size 12KB take up more than 100MB in total, and 100MB is the in-memory limit for $sort
        // and $group.
        docSize: 12 * 1000
    };

    var getStringOfLength = (function() {
        var cache = {};
        return function getStringOfLength(size) {
            if (!cache[size]) {
                cache[size] = new Array(size + 1).join('x');
            }
            return cache[size];
        };
    })();

    function padDoc(doc, size) {
        // first set doc.padding so that Object.bsonsize will include the field name and other
        // overhead
        doc.padding = "";
        var paddingLength = size - Object.bsonsize(doc);
        assertAlways.lte(
            0, paddingLength, 'document is already bigger than ' + size + ' bytes: ' + tojson(doc));
        doc.padding = getStringOfLength(paddingLength);
        assertAlways.eq(size, Object.bsonsize(doc));
        return doc;
    }

    var states = {
        query: function query(db, collName) {
            var count = db[collName].aggregate([]).itcount();
            assertWhenOwnColl.eq(count, this.numDocs);
        }
    };

    var transitions = {query: {query: 1}};

    function setup(db, collName, cluster) {
        // load example data
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            // note: padDoc caches the large string after allocating it once, so it's ok to call it
            // in this loop
            bulk.insert(padDoc({
                flag: i % 2 ? true : false,
                rand: Random.rand(),
                randInt: Random.randInt(this.numDocs)
            },
                               this.docSize));
        }
        var res = bulk.execute();
        assertWhenOwnColl.writeOK(res);
        assertWhenOwnColl.eq(this.numDocs, res.nInserted);
        assertWhenOwnColl.eq(this.numDocs, db[collName].find().itcount());
        assertWhenOwnColl.eq(this.numDocs / 2, db[collName].find({flag: false}).itcount());
        assertWhenOwnColl.eq(this.numDocs / 2, db[collName].find({flag: true}).itcount());
    }

    function teardown(db, collName, cluster) {
        assertWhenOwnColl(db[collName].drop());
    }

    return {
        // Using few threads and iterations because each iteration is fairly expensive compared to
        // other workloads' iterations. (Each does a collection scan over a few thousand documents
        // rather than a few dozen documents.)
        threadCount: 5,
        iterations: 10,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown
    };
})();
