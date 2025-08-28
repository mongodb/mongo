/**
 * distinct.js
 *
 * Runs distinct on an indexed field and verifies the result.
 * The indexed field contains unique values.
 * Each thread operates on a separate collection.
 *
 */
export const $config = (function () {
    let data = {numDocs: 1000, prefix: "distinct_fsm", shardKey: {i: 1}};

    let states = (function () {
        function init(db, collName) {
            this.threadCollName = this.prefix + "_" + this.tid;
            let bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({i: i});
            }
            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.numDocs, res.nInserted);
            assert.commandWorked(db[this.threadCollName].createIndex({i: 1}));
        }

        function distinct(db, collName) {
            try {
                assert.eq(this.numDocs, db[this.threadCollName].distinct("i").length);
            } catch (e) {
                // Range deletion completing may (correctly) cause this query plan to be killed.
                // TODO SERVER-97712: On transaction passthroughs this may fail with
                // ExceededTimeLimit.
                assert(
                    [ErrorCodes.QueryPlanKilled, ErrorCodes.ExceededTimeLimit].includes(e.code),
                    "Expected a QueryPlanKilled or ExceededTimeLimit error, but encountered: " + e.message,
                );
            }
        }

        return {init: init, distinct: distinct};
    })();

    let transitions = {init: {distinct: 1}, distinct: {distinct: 1}};

    return {
        data: data,
        threadCount: 5,
        iterations: 10,
        states: states,
        transitions: transitions,
    };
})();
