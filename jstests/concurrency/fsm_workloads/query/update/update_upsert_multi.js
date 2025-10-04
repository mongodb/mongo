/**
 * update_upsert_multi.js
 *
 * Tests updates that specify both multi=true and upsert=true.
 * The 'insert' state uses a query that will match no documents, causing an upsert.
 * The 'update' state uses a query that will match one or more documents, causing a multi-update.
 * Both states use { multi: true, upsert: true }, but only one option will ever take effect,
 * depending on whether 0 or more than 0 documents match the query.
 *
 * @tags: [requires_non_retryable_writes]
 */

export const $config = (function () {
    let states = {
        insert: function insert(db, collName) {
            let query, update, options;
            let res = db[collName].update(
                // The counter ensures that the query will not match any existing document.
                (query = {tid: this.tid, i: this.counter++}),
                (update = {$inc: {n: 1}}),
                (options = {multi: true, upsert: true}),
            );
            let debugDoc = tojson({query: query, update: update, options: options, result: res});
            assert.eq(1, res.nUpserted, debugDoc);
            assert.eq(0, res.nMatched, debugDoc);
            assert.eq(0, res.nModified, debugDoc);
        },

        update: function update(db, collName) {
            // moveChunk can kill the multiupdate command and return QueryPlanKilled, so retry in
            // that case.
            retryOnRetryableError(
                () => {
                    const res = db[collName].update(
                        // This query will match an existing document, since the 'insert' state
                        // always runs first.
                        {tid: this.tid},
                        {$inc: {n: 1}},
                        {multi: true, upsert: true},
                    );

                    if (res instanceof WriteResult && res.hasWriteError()) {
                        throw _getErrorWithCode(res, "Update failed: " + tojson(res));
                    } else if (!res.ok) {
                        assert.commandWorked(res);
                    }

                    assert.eq(0, res.nUpserted, tojson(res));
                    assert.lte(1, res.nMatched, tojson(res));
                    assert.eq(res.nMatched, res.nModified, tojson(res));
                },
                100,
                undefined,
                TestData.runningWithBalancer ? [ErrorCodes.QueryPlanKilled] : [],
            );
        },

        assertConsistency: function assertConsistency(db, collName) {
            // Since each update operation either:
            //  - inserts a new doc { tid: tid, i: counter++, n: 0 }
            //  - updates any doc matching { tid: tid } with { $inc: { n: 1 } }
            // Then within each tid, as you walk from lower to higher values of i,
            // the value of n should be non-increasing. (This should be true
            // because docs with lower i are newer, so they have had fewer
            // opportunities to have n incremented.)
            let prevN = Infinity;
            db[collName]
                .find({tid: this.tid})
                .sort({i: 1})
                .forEach(function (doc) {
                    assert.gte(prevN, doc.n);
                    prevN = doc.n;
                });
        },
    };

    let transitions = {
        insert: {update: 0.875, assertConsistency: 0.125},
        update: {insert: 0.875, assertConsistency: 0.125},
        assertConsistency: {insert: 0.5, update: 0.5},
    };

    function setup(db, collName, cluster) {
        assert.commandWorked(db[collName].createIndex({tid: 1, i: 1}));
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        startState: "insert",
        transitions: transitions,
        // Shard by tid when run in a sharded cluster because upserts require the shard key.
        data: {counter: 0, shardKey: {tid: 1}},
        setup: setup,
    };
})();
