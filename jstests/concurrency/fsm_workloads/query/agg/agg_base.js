/**
 * agg_base.js
 *
 * Base workload for aggregation. Inserts a bunch of documents in its setup,
 * then each thread does an aggregation with an empty $match.
 * @tags: [
 *   requires_getmore,
 *   # Some passthrough suites can tolerate getMore commands because the passthrough wraps them in
 *   # transactions. The getMore in this workload does not get wrapped, though, because it is in the
 *   # setup function, making the workload incompatible with even those suites.
 *   uses_getmore_outside_of_transaction,
 * ]
 */

import {isEphemeral} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let getStringOfLength = (function () {
        let cache = {};
        return function getStringOfLength(size) {
            if (!cache[size]) {
                cache[size] = "x".repeat(size);
            }
            return cache[size];
        };
    })();

    function padDoc(doc, size) {
        // first set doc.padding so that Object.bsonsize will include the field name and other
        // overhead
        doc.padding = "";
        let paddingLength = size - Object.bsonsize(doc);
        assert.lte(0, paddingLength, "document is already bigger than " + size + " bytes: " + tojson(doc));
        doc.padding = getStringOfLength(paddingLength);
        assert.eq(size, Object.bsonsize(doc));
        return doc;
    }

    let states = {
        query: function query(db, collName) {
            let count = db[collName].aggregate([]).itcount();
            assert.eq(count, this.numDocs);
        },
    };

    let transitions = {query: {query: 1}};

    function setup(db, collName, cluster) {
        if (!this.numDocs) {
            this.numDocs = 1000;
        }
        if (!this.docSize) {
            // Use 12KB documents by default. This number is useful because 12,000 documents each of
            // size 12KB take up more than 100MB in total, and 100MB is the in-memory limit for
            // $sort and $group.
            this.docSize = 12 * 1000;
        }
        this.anyNodeIsEphemeral = false;

        // TODO SERVER-92452: Burn in testing fails with WT_CACHE_FULL for inmemory variants, so
        // substantially reduce the workload until this is fixed in a better way.
        cluster.executeOnMongodNodes((db) => {
            this.anyNodeIsEphemeral = this.anyNodeIsEphemeral || isEphemeral(db);
        });
        if (this.anyNodeIsEphemeral) {
            this.numDocs = this.numDocs / 100;
            this.docSize = Math.max(this.docSize / 100, 100);
        }

        // load example data
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            // note: padDoc caches the large string after allocating it once, so it's ok to call it
            // in this loop
            bulk.insert(
                padDoc(
                    {
                        flag: i % 2 ? true : false,
                        rand: Random.rand(),
                        randInt: Random.randInt(this.numDocs),
                    },
                    this.docSize,
                ),
            );
        }
        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, db[collName].find().itcount());
        assert.eq(this.numDocs / 2, db[collName].find({flag: false}).itcount());
        assert.eq(this.numDocs / 2, db[collName].find({flag: true}).itcount());
    }

    function teardown(db, collName, cluster) {
        // By default, do nothing on teardown. Workload tests may implement this function.
    }

    return {
        // Using few threads and iterations because each iteration is fairly expensive compared to
        // other workloads' iterations. (Each does a collection scan over a few thousand documents
        // rather than a few dozen documents.)
        threadCount: 5,
        iterations: 10,
        states: states,
        startState: "query",
        transitions: transitions,
        data: {},
        setup: setup,
        teardown: teardown,
    };
})();
